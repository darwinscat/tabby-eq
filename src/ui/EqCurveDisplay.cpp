// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/EqCurveDisplay.h"
#include "ui/Palette.h"
#include "ui/FilterShapes.h"

namespace
{
    bool hasGain (teq::FilterType t) noexcept
    {
        return t == teq::FilterType::Bell     || t == teq::FilterType::LowShelf
            || t == teq::FilterType::HighShelf || t == teq::FilterType::Tilt;
    }

    bool qRelevant (teq::FilterType t) noexcept   // Q sets bandwidth -> show the Q whiskers
    {                                             // HP/LP use discrete slopes (their own whisker), not Q
        return t == teq::FilterType::Bell  || t == teq::FilterType::BandPass
            || t == teq::FilterType::Notch || t == teq::FilterType::AllPass;
    }

    // Q-whisker calibration — half-bandwidth (octaves) <-> Q, log-linear. Tuned by feel so the
    // handles span a usable range: 0.2 oct out = max Q 40, 0.833 oct out = min Q 0.1.
    constexpr double kQwBwLo = 0.2, kQwBwHi = 0.833, kQwQHi = 40.0, kQwQLo = 0.1;
    double whiskerBwForQ (double Q) noexcept
    {
        const double t = (std::log10 (kQwQHi) - std::log10 (juce::jlimit (kQwQLo, kQwQHi, Q)))
                       / (std::log10 (kQwQHi) - std::log10 (kQwQLo));
        return kQwBwLo + t * (kQwBwHi - kQwBwLo);
    }
    double whiskerQForBw (double bw) noexcept
    {
        const double t = juce::jlimit (0.0, 1.0, (bw - kQwBwLo) / (kQwBwHi - kQwBwLo));
        return std::pow (10.0, std::log10 (kQwQHi) - t * (std::log10 (kQwQHi) - std::log10 (kQwQLo)));
    }

    bool isCut (teq::FilterType t) noexcept { return t == teq::FilterType::HighPass || t == teq::FilterType::LowPass; }
    bool whiskerRelevant (teq::FilterType t) noexcept { return qRelevant (t) || isCut (t); }

    // HP/LP whisker maps to the DISCRETE slope list (steeper = narrower handle) instead of Q.
    constexpr int kSlopeDb[7] = { 6, 12, 24, 36, 48, 72, 96 };
    int    slopeIndexFromDb (int db) noexcept { for (int i = 0; i < 7; ++i) if (kSlopeDb[i] == db) return i; return 1; }
    double slopeBwForIndex  (int i)  noexcept { return kQwBwHi + (kQwBwLo - kQwBwHi) * (i / 6.0); }   // i0 wide -> i6 narrow
    int    slopeIndexForBw  (double bw) noexcept
    {
        const double t = juce::jlimit (0.0, 1.0, (kQwBwHi - bw) / (kQwBwHi - kQwBwLo));
        return juce::jlimit (0, 6, (int) std::round (t * 6.0));
    }

    juce::Colour bandColour (teq::FilterType t) noexcept
    {
        using FT = teq::FilterType;
        switch (t)
        {
            case FT::LowShelf:  case FT::HighShelf: return juce::Colour (0xff7fc8ff);   // blue
            case FT::HighPass:  case FT::LowPass:   return juce::Colour (0xffff9d5c);   // orange
            case FT::BandPass:                      return juce::Colour (0xffb98cff);   // violet
            case FT::Notch:                         return juce::Colour (0xffff6b8a);   // red
            case FT::AllPass:                       return juce::Colour (0xff9aa6b8);   // grey
            case FT::Tilt:                          return juce::Colour (0xff5cd6c8);   // teal
            case FT::Bell:      default:            return juce::Colour (0xff6ee7a8);   // green
        }
    }
}

EqCurveDisplay::EqCurveDisplay (TabbyEqAudioProcessor& p) : proc (p)
{
    for (int i = 0; i < (int) window.size(); ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (window.size() - 1));

    specDb.fill (-120.0f);
    specPeak.fill (-120.0f);
    proc.setAnalyzerActive (true);
    startTimerHz (30);
}

EqCurveDisplay::~EqCurveDisplay()
{
    stopTimer();
    endDragGesture();                 // never leave a host automation gesture open
    proc.setAnalyzerActive (false);
}

//==============================================================================
float  EqCurveDisplay::freqToX (double f) const noexcept
{
    const double lo = std::log10 (kFreqMin), hi = std::log10 (kFreqMax);
    return (float) ((std::log10 (juce::jlimit (kFreqMin, kFreqMax, f)) - lo) / (hi - lo) * (double) getWidth());
}

double EqCurveDisplay::xToFreq (float x) const noexcept
{
    const double lo = std::log10 (kFreqMin), hi = std::log10 (kFreqMax);
    const double t = juce::jlimit (0.0, 1.0, (double) x / (double) juce::jmax (1, getWidth()));
    return std::pow (10.0, lo + t * (hi - lo));
}

float  EqCurveDisplay::dbToY (double db) const noexcept
{
    return (float) ((0.5 - db / (2.0 * kGainRange)) * (double) getHeight());
}

double EqCurveDisplay::yToDb (float y) const noexcept
{
    return (0.5 - (double) y / (double) juce::jmax (1, getHeight())) * (2.0 * kGainRange);
}

float  EqCurveDisplay::specDbToY (double db) const noexcept
{
    const double t = juce::jlimit (0.0, 1.0, (kSpecTop - db) / (kSpecTop - kSpecBottom));
    return (float) (t * (double) getHeight());
}

//==============================================================================
void EqCurveDisplay::refreshDesigns()
{
    fsCache = proc.getSampleRate() > 0.0 ? proc.getSampleRate() : 44100.0;
    for (int b = 0; b < tabby::kNumBands; ++b)
    {
        paramCache[b]  = proc.readBand (b);
        designCache[b] = paramCache[b].on ? teq::designBand (paramCache[b], fsCache) : teq::BandDesign {};
    }
}

double EqCurveDisplay::compositeDb (double f) const noexcept
{
    const double w = 2.0 * juce::MathConstants<double>::pi * f / fsCache;
    std::complex<double> h { 1.0, 0.0 };
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (paramCache[b].on && ! paramCache[b].bypass)
            for (int s = 0; s < designCache[b].n; ++s)
                h *= teq::evalCoeffs (designCache[b].sec[s], w);
    return 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (h)));
}

juce::Point<float> EqCurveDisplay::nodePos (int b) const noexcept
{
    // bells/shelves/tilt sit at their OWN gain (a drag tracks the cursor); notch/all-pass are
    // surgical / phase-only so they sit on the 0 dB line; HP/LP/BP ride the composite at the corner.
    const auto t = paramCache[b].type;
    double db;
    if (hasGain (t))                                                       db = paramCache[b].gainDb;
    else if (t == teq::FilterType::Notch || t == teq::FilterType::AllPass) db = 0.0;
    else                                                                   db = compositeDb (paramCache[b].freq);
    return { freqToX (paramCache[b].freq), dbToY (db) };
}

std::pair<juce::Point<float>, juce::Point<float>> EqCurveDisplay::whiskerEnds (int b) const noexcept
{
    const auto pos = nodePos (b);
    const double f0 = paramCache[(size_t) b].freq;
    const auto   t  = paramCache[(size_t) b].type;
    const double bw = isCut (t) ? slopeBwForIndex (slopeIndexFromDb (paramCache[(size_t) b].slope))
                                : whiskerBwForQ (paramCache[(size_t) b].Q);   // half-bandwidth (octaves)
    const float dx  = freqToX (f0 * std::exp2 (bw)) - pos.x;                  // -> px offset (log-x, symmetric)
    return { { pos.x - dx, pos.y }, { pos.x + dx, pos.y } };
}

int EqCurveDisplay::nodeAt (juce::Point<float> p) const noexcept
{
    int best = -1; float bestD = (kNodeR + 5.0f) * (kNodeR + 5.0f);
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (paramCache[b].on)
        {
            const float d = p.getDistanceSquaredFrom (nodePos (b));
            if (d < bestD) { bestD = d; best = b; }
        }
    return best;
}

void EqCurveDisplay::setParam (const juce::String& id, double value)
{
    if (auto* prm = proc.apvts.getParameter (id))
        prm->setValueNotifyingHost (prm->convertTo0to1 ((float) value));
}

void EqCurveDisplay::setParamGestured (const juce::String& id, double value)
{
    if (auto* prm = proc.apvts.getParameter (id))
    {
        prm->beginChangeGesture();
        prm->setValueNotifyingHost (prm->convertTo0to1 ((float) value));
        prm->endChangeGesture();
    }
}

void EqCurveDisplay::endDragGesture()
{
    if (draggingBand >= 0)
    {
        if (draggingQ)
        {
            if (auto* prm = proc.apvts.getParameter (tabby::bandId (draggingBand, whiskerSlope ? "slope" : "q")))
                prm->endChangeGesture();
        }
        else
        {
            if (auto* fp = proc.apvts.getParameter (tabby::bandId (draggingBand, "freq"))) fp->endChangeGesture();
            if (draggingGain)
                if (auto* gp = proc.apvts.getParameter (tabby::bandId (draggingBand, "gain"))) gp->endChangeGesture();
        }
    }
    draggingBand = -1;
    draggingGain = false;
    draggingQ    = false;
    qDragSide    = 0;
    whiskerSlope = false;
}

void EqCurveDisplay::selectBand (int newSel)
{
    if (newSel == selBand) return;
    selBand = newSel;
    if (onBandSelected) onBandSelected (selBand);
}

juce::String EqCurveDisplay::readoutText (int b) const
{
    const auto& p = paramCache[(size_t) b];
    const double f = p.freq;
    juce::String s = f >= 1000.0 ? juce::String (f / 1000.0, 2) + " kHz"
                                 : juce::String (juce::roundToInt (f)) + " Hz";
    if (hasGain (p.type))
        s << "   " << (p.gainDb >= 0.0 ? "+" : "") << juce::String (p.gainDb, 1) << " dB";
    const bool showsQ = ! (p.type == teq::FilterType::LowShelf || p.type == teq::FilterType::HighShelf
                           || p.type == teq::FilterType::Tilt);                          // shelves & tilt ignore Q
    if (showsQ)
        s << "   Q " << juce::String (p.Q, 2);
    return s;
}

//==============================================================================
void EqCurveDisplay::pushSpectrum()
{
    if (! proc.pullSpectrum (analyzerPre, fftBuf.data()))
    {
        // No new frame this tick is NORMAL (a 2048-pt window arrives at ~23 fps vs the 30 Hz timer).
        // Hold the last spectrum; only fade once genuinely starved (audio stopped ~0.5 s).
        if (starveTicks < 16) ++starveTicks;          // bounded — never overflows over long silence
        if (starveTicks > 15)
            for (auto& v : specDb) v += 0.05f * (-120.0f - v);
        for (auto& v : specPeak) v = juce::jmax (-120.0f, v - kPeakFallDb);
        return;
    }
    starveTicks = 0;

    for (int i = 0; i < teq::kSpectrumFftSize; ++i) fftBuf[(size_t) i] *= window[(size_t) i];
    std::fill (fftBuf.begin() + teq::kSpectrumFftSize, fftBuf.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftBuf.data());

    const double norm = (double) teq::kSpectrumFftSize * 0.25;   // Hann single-bin compensation
    for (int i = 0; i < (int) specDb.size(); ++i)
    {
        const double db = juce::Decibels::gainToDecibels ((double) fftBuf[(size_t) i] / norm, -120.0);
        specDb[(size_t) i]  += 0.25f * ((float) db - specDb[(size_t) i]);   // smooth toward target
        specPeak[(size_t) i] = juce::jmax (specPeak[(size_t) i] - kPeakFallDb, specDb[(size_t) i]);
    }
}

// Spectrum on the display's log grid: linear-interpolate the sparse bass (smooth, no stair-steps),
// take the bin MAX in the dense highs (keeps the spiky harmonic detail pro analyzers show). High
// column resolution (~1/px) so high-freq peaks aren't skipped. +4.5 dB/oct pink-tilt on top.
void EqCurveDisplay::buildSpectrumPaths (juce::Path& fillOut, juce::Path& peakOut, float w, float h) const
{
    constexpr int nb = teq::kSpectrumFftSize / 2 + 1;
    const double binPerHz = (double) teq::kSpectrumFftSize / fsCache;
    const int N = juce::jlimit (256, 900, (int) w);

    auto column = [&] (const auto& raw, double f, int loBin, int hiBin) -> float
    {
        if (hiBin <= loBin)                                         // < 1 bin this column -> interpolate
        {
            const double bf = juce::jlimit (0.0, (double) (nb - 2), f * binPerHz);
            const int b0 = (int) bf; const float t = (float) (bf - (double) b0);
            return raw[(size_t) b0] + t * (raw[(size_t) (b0 + 1)] - raw[(size_t) b0]);
        }
        float m = -200.0f;                                          // >= 1 bin -> peak (max) = detail
        for (int b = juce::jmax (0, loBin + 1); b <= juce::jmin (nb - 1, hiBin); ++b)
            m = juce::jmax (m, raw[(size_t) b]);
        return m;
    };

    float lastY = h;
    int prevBin = 0;
    for (int i = 0; i <= N; ++i)
    {
        const float  x = (float) i / (float) N * w;
        const double f = xToFreq (x);
        const int    curBin = juce::jlimit (0, nb - 1, (int) std::floor (f * binPerHz));
        const float  tilt = (float) (kTiltDbPerOct * std::log2 (f / kTiltPivotHz));   // pink-noise comp
        const float  yS = specDbToY (column (specDb,   f, prevBin, curBin) + tilt);
        const float  yP = specDbToY (column (specPeak, f, prevBin, curBin) + tilt);
        if (i == 0) { fillOut.startNewSubPath (0.0f, h); fillOut.lineTo (0.0f, yS); peakOut.startNewSubPath (x, yP); }
        else          peakOut.lineTo (x, yP);
        fillOut.lineTo (x, yS);
        prevBin = curBin;
        lastY = yS;
    }
    fillOut.lineTo (w, lastY); fillOut.lineTo (w, h); fillOut.closeSubPath();
}

void EqCurveDisplay::timerCallback()
{
    pushSpectrum();
    repaint();
}

//==============================================================================
void EqCurveDisplay::paint (juce::Graphics& g)
{
    const auto w = (float) getWidth(), h = (float) getHeight();
    refreshDesigns();

    // premium radial vignette: centre lifted a touch, corners deepened
    {
        const auto bb = getLocalBounds().toFloat();
        juce::ColourGradient vg (tabby::palette::bg().brighter (0.18f), bb.getCentreX(), bb.getCentreY() - h * 0.06f,
                                 tabby::palette::bg().darker (0.55f),   bb.getX(),       bb.getBottom(), true);
        g.setGradientFill (vg);
        g.fillRect (bb);
    }

    // --- grid -------------------------------------------------------------
    g.setFont (11.0f);
    for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const float x = freqToX (f);
        g.setColour (tabby::palette::grid());
        g.drawVerticalLine ((int) x, 0.0f, h);
        g.setColour (tabby::palette::axisText());
        g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, (f == 1000.0 ? 0 : 0)) + "k" : juce::String ((int) f),
                    (int) x + 2, (int) h - 14, 34, 12, juce::Justification::left);
    }
    for (double db : { -18.0, -12.0, -6.0, 0.0, 6.0, 12.0, 18.0 })
    {
        const float y = dbToY (db);
        if (db == 0.0)
        {
            g.setColour (tabby::palette::violetLo().withAlpha (0.05f));   // soft glow on the 0 dB line
            g.fillRect (0.0f, y - 2.5f, w, 5.0f);
            g.setColour (tabby::palette::gridZero());
        }
        else
            g.setColour (tabby::palette::grid());
        g.drawHorizontalLine ((int) y, 0.0f, w);
    }

    // --- spectrum (filled) ------------------------------------------------
    {
        juce::Path specFill, specPeakPath;
        buildSpectrumPaths (specFill, specPeakPath, w, h);
        g.setGradientFill (juce::ColourGradient (tabby::palette::spectrum().withAlpha (0.22f), 0.0f, h * 0.30f,
                                                 tabby::palette::spectrum().withAlpha (0.03f), 0.0f, h, false));
        g.fillPath (specFill);
        g.setColour (tabby::palette::spectrum().withAlpha (0.55f));
        g.strokePath (specPeakPath, juce::PathStrokeType (1.0f));
    }

    // --- response curve: warm/cool fill to 0 dB, faux-glow, crisp line ----
    {
        constexpr int pts = 240;
        const float y0 = dbToY (0.0);
        juce::Path line, fill;
        fill.startNewSubPath (0.0f, y0);
        for (int i = 0; i <= pts; ++i)
        {
            const float x = (float) i / (float) pts * w;
            const float y = dbToY (compositeDb (xToFreq (x)));
            if (i == 0) line.startNewSubPath (x, y); else line.lineTo (x, y);
            fill.lineTo (x, y);
        }
        fill.lineTo (w, y0);
        fill.closeSubPath();

        // boost (above 0 dB) reads warm = orange; cut (below) reads cool = violet. One fill path,
        // clipped to each half. Violet gets more alpha — it's the dimmer of the two brand colours.
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (0, 0, (int) w, juce::jmax (0, (int) y0));
            g.setColour (tabby::palette::orange().withAlpha (0.14f));
            g.fillPath (fill);
        }
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (0, (int) y0, (int) w, juce::jmax (0, (int) (h - y0)));
            g.setColour (tabby::palette::violet().withAlpha (0.20f));
            g.fillPath (fill);
        }

        // faux-glow: stacked low-alpha wide strokes in lifted violet (NO real blur), then the line
        g.setColour (tabby::palette::violetLo().withAlpha (0.10f)); g.strokePath (line, juce::PathStrokeType (6.0f));
        g.setColour (tabby::palette::violetLo().withAlpha (0.20f)); g.strokePath (line, juce::PathStrokeType (3.0f));
        g.setColour (tabby::palette::line());                       g.strokePath (line, juce::PathStrokeType (1.6f));
    }

    // --- hover halo on the node under the cursor --------------------------
    if (hoverPos.x >= 0.0f && draggingBand < 0)
    {
        const int hb = nodeAt (hoverPos);
        if (hb >= 0)
        {
            const auto hp = nodePos (hb);
            g.setColour (bandColour (paramCache[(size_t) hb].type).withAlpha (0.22f));
            g.fillEllipse (hp.x - kNodeR - 7, hp.y - kNodeR - 7, (kNodeR + 7) * 2, (kNodeR + 7) * 2);
        }
    }

    // --- nodes ------------------------------------------------------------
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (paramCache[b].on)
        {
            const auto pos = nodePos (b);
            const auto col = bandColour (paramCache[b].type);
            const auto numR = juce::Rectangle<float> (pos.x - kNodeR, pos.y - kNodeR, kNodeR * 2, kNodeR * 2);
            if (paramCache[b].bypass)   // ghost: dim hollow ring — still hit-tested, so it's clickable back on
            {
                g.setColour (col.withAlpha (0.45f));
                g.drawEllipse (pos.x - kNodeR, pos.y - kNodeR, kNodeR * 2, kNodeR * 2, 1.5f);
                g.setColour (juce::Colours::white.withAlpha (0.45f)); g.setFont (10.0f);
                g.drawText (juce::String (b + 1), numR, juce::Justification::centred);
            }
            else
            {
                g.setColour (col.withAlpha (0.25f)); g.fillEllipse (pos.x - kNodeR - 2, pos.y - kNodeR - 2, (kNodeR + 2) * 2, (kNodeR + 2) * 2);
                g.setColour (col);                   g.fillEllipse (pos.x - kNodeR,     pos.y - kNodeR,     kNodeR * 2,       kNodeR * 2);
                g.setColour (juce::Colours::black.withAlpha (0.5f)); g.drawEllipse (pos.x - kNodeR, pos.y - kNodeR, kNodeR * 2, kNodeR * 2, 1.0f);
                g.setColour (juce::Colours::white); g.setFont (10.0f);
                g.drawText (juce::String (b + 1), numR, juce::Justification::centred);
            }
        }

    // --- selection highlight + live value bubble --------------------------
    const int rb = draggingBand >= 0 ? draggingBand : selBand;
    if (rb >= 0 && rb < tabby::kNumBands && paramCache[(size_t) rb].on)
    {
        const auto pos = nodePos (rb);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawEllipse (pos.x - kNodeR - 3, pos.y - kNodeR - 3, (kNodeR + 3) * 2, (kNodeR + 3) * 2, 1.5f);

        // Whiskers (Neutron-style): drag an end handle to set bandwidth/Q (HP/LP -> discrete slope)
        if (whiskerRelevant (paramCache[(size_t) rb].type))
        {
            const auto wk  = whiskerEnds (rb);
            const auto col = bandColour (paramCache[(size_t) rb].type);
            g.setColour (col.withAlpha (0.85f));
            g.drawLine (wk.first.x, pos.y, wk.second.x, pos.y, 1.5f);
            for (const auto& hp : { wk.first, wk.second })
            {
                g.setColour (tabby::palette::bg()); g.fillEllipse (hp.x - 4.0f, hp.y - 4.0f, 8.0f, 8.0f);
                g.setColour (col);                  g.drawEllipse (hp.x - 4.0f, hp.y - 4.0f, 8.0f, 8.0f, 1.5f);
            }
        }

        const juce::String txt = readoutText (rb);
        const float tw = (float) txt.length() * 6.6f + 12.0f, th = 18.0f;
        const float bx = juce::jlimit (2.0f, w - tw - 2.0f, pos.x - tw * 0.5f);
        const float by = juce::jlimit (2.0f, h - th - 2.0f, pos.y - kNodeR - 8.0f - th);
        g.setColour (tabby::palette::panel().withAlpha (0.96f));    g.fillRoundedRectangle (bx, by, tw, th, 4.0f);
        g.setColour (tabby::palette::violetLo().withAlpha (0.55f)); g.drawRoundedRectangle (bx, by, tw, th, 4.0f, 1.0f);
        g.setColour (tabby::palette::text()); g.setFont (12.0f);
        g.drawText (txt, juce::Rectangle<float> (bx, by, tw, th), juce::Justification::centred);
    }

    // --- "+" add-band button where the cursor crosses the curve ----------
    const auto addBtn = addButtonAt();
    if (addBtn.x >= 0.0f)
    {
        const float r = kNodeR + 2.0f;
        g.setColour (tabby::palette::panel().withAlpha (0.92f));    g.fillEllipse (addBtn.x - r, addBtn.y - r, r * 2, r * 2);
        g.setColour (tabby::palette::violetLo().withAlpha (0.85f)); g.drawEllipse (addBtn.x - r, addBtn.y - r, r * 2, r * 2, 1.5f);
        g.setColour (tabby::palette::text());
        g.drawLine (addBtn.x - 4.0f, addBtn.y, addBtn.x + 4.0f, addBtn.y, 1.6f);
        g.drawLine (addBtn.x, addBtn.y - 4.0f, addBtn.x, addBtn.y + 4.0f, 1.6f);
    }
}

//==============================================================================
void EqCurveDisplay::addBandOfType (int typeIndex, juce::Point<float> at)
{
    refreshDesigns();
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (! paramCache[b].on)
        {
            const auto ft = tabby::filterTypeFromChoice (typeIndex);
            setParamGestured (tabby::bandId (b, "type"), typeIndex);
            setParamGestured (tabby::bandId (b, "freq"), xToFreq (at.x));
            if (ft == teq::FilterType::Bell || ft == teq::FilterType::LowShelf || ft == teq::FilterType::HighShelf)
                setParamGestured (tabby::bandId (b, "gain"), juce::jlimit (-kGainRange, kGainRange, yToDb (at.y)));
            setParamGestured (tabby::bandId (b, "q"), (ft == teq::FilterType::HighPass || ft == teq::FilterType::LowPass) ? 0.707 : 1.0);
            setParamGestured (tabby::bandId (b, "bypass"), 0.0);
            setParamGestured (tabby::bandId (b, "on"), 1.0);
            selectBand (b);
            return;
        }
}

void EqCurveDisplay::mouseDown (const juce::MouseEvent& e)
{
    refreshDesigns();
    const int b = nodeAt (e.position);

    if (e.mods.isPopupMenu())
    {
        const char* names[] = { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Band Pass", "Notch", "All Pass", "Tilt" };
        juce::Component::SafePointer<EqCurveDisplay> safe (this);
        juce::PopupMenu m;

        if (b >= 0)   // on a node: change type / remove
        {
            for (int i = 0; i < 9; ++i)
            {
                juce::PopupMenu::Item it;
                it.itemID = i + 1;
                it.text   = names[i];
                it.setImage (tabby::shapes::icon (tabby::filterTypeFromChoice (i), tabby::palette::violetLo()));
                it.setTicked (paramCache[b].type == tabby::filterTypeFromChoice (i));
                m.addItem (it);
            }
            m.addSeparator();
            m.addItem (100, "Remove band");
            m.showMenuAsync (juce::PopupMenu::Options(), [safe, b] (int r) {
                if (safe == nullptr || r == 0) return;
                if (r == 100) safe->setParamGestured (tabby::bandId (b, "on"), 0.0);
                else          safe->setParamGestured (tabby::bandId (b, "type"), r - 1);
            });
        }
        else          // empty: add a band here, if a slot is free
        {
            bool anyFree = false;
            for (int i = 0; i < tabby::kNumBands; ++i) if (! paramCache[i].on) { anyFree = true; break; }

            if (anyFree)
            {
                juce::PopupMenu add;
                for (int i = 0; i < 9; ++i)
                {
                    juce::PopupMenu::Item it;
                    it.itemID = i + 1;
                    it.text   = names[i];
                    it.setImage (tabby::shapes::icon (tabby::filterTypeFromChoice (i), tabby::palette::violetLo()));
                    add.addItem (it);
                }
                m.addSubMenu ("Add band", add);
            }
            else
                m.addItem (999, "All 24 bands in use", false, false);

            const auto at = e.position;
            m.showMenuAsync (juce::PopupMenu::Options(), [safe, at] (int r) {
                if (safe != nullptr && r > 0 && r < 100) safe->addBandOfType (r - 1, at);
            });
        }
        return;
    }

    // "+" on the curve under the cursor -> add a flat Bell at that frequency
    const auto addBtn = addButtonAt();
    if (addBtn.x >= 0.0f && e.position.getDistanceFrom (addBtn) < kNodeR + 6.0f)
    {
        addBandOfType (0, { addBtn.x, dbToY (0.0) });
        return;
    }

    // A drag on the selected band's Q-whisker handle sets Q (Neutron-style), keeping the selection.
    if (selBand >= 0 && b < 0 && paramCache[(size_t) selBand].on && whiskerRelevant (paramCache[(size_t) selBand].type))
    {
        const auto wk = whiskerEnds (selBand);
        const bool onRight = e.position.getDistanceFrom (wk.second) < kNodeR + 4.0f;
        const bool onLeft  = e.position.getDistanceFrom (wk.first)  < kNodeR + 4.0f;
        if (onLeft || onRight)
        {
            draggingBand = selBand; draggingQ = true; qDragSide = onRight ? 1 : -1;
            whiskerSlope = isCut (paramCache[(size_t) selBand].type);
            if (auto* prm = proc.apvts.getParameter (tabby::bandId (selBand, whiskerSlope ? "slope" : "q")))
                prm->beginChangeGesture();
            return;
        }
    }

    // select the clicked node (or clear on empty) so the edit strip follows, then start dragging
    selectBand (b);
    draggingBand = b;
    if (b >= 0)
    {
        if (auto* fp = proc.apvts.getParameter (tabby::bandId (b, "freq"))) fp->beginChangeGesture();
        draggingGain = hasGain (paramCache[b].type);
        if (draggingGain)
            if (auto* gp = proc.apvts.getParameter (tabby::bandId (b, "gain"))) gp->beginChangeGesture();
    }
}

void EqCurveDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (draggingBand < 0) return;
    if (draggingQ)      // Neutron-style Q-whisker drag: handle distance from centre -> bandwidth -> Q
    {
        const double f0 = paramCache[(size_t) draggingBand].freq;
        const double fx = xToFreq (e.position.x);
        // Clamp the grabbed handle to its own side of the node (no jumping across), then map the
        // half-bandwidth to Q with the calibrated curve (hits 40 / 0.1 at the ends).
        const double bw = (qDragSide > 0) ? std::log2 (juce::jmax (f0, fx) / f0)
                                          : std::log2 (f0 / juce::jmin (f0, fx));
        if (whiskerSlope)   // HP/LP -> snap to the discrete slope list
            setParam (tabby::bandId (draggingBand, "slope"), (double) slopeIndexForBw (bw));
        else                // others -> continuous Q (calibrated, hits 40 / 0.1 at the ends)
            setParam (tabby::bandId (draggingBand, "q"), juce::jlimit (0.1, 40.0, whiskerQForBw (bw)));
        return;
    }
    setParam (tabby::bandId (draggingBand, "freq"), xToFreq (e.position.x));
    if (draggingGain)   // latched at mouseDown so the gain begin/end gesture stays balanced
        setParam (tabby::bandId (draggingBand, "gain"), juce::jlimit (-kGainRange, kGainRange, yToDb (e.position.y)));
}

void EqCurveDisplay::mouseUp (const juce::MouseEvent&) { endDragGesture(); }

void EqCurveDisplay::mouseMove (const juce::MouseEvent& e) { hoverPos = e.position; repaint(); }
void EqCurveDisplay::mouseExit (const juce::MouseEvent&)   { hoverPos = { -1.0f, -1.0f }; repaint(); }

juce::Point<float> EqCurveDisplay::addButtonAt() const noexcept
{
    if (hoverPos.x < 0.0f || draggingBand >= 0)     return { -1.0f, -1.0f };
    if (nodeAt (hoverPos) >= 0)                      return { -1.0f, -1.0f };   // on a node -> no "+"

    // don't surface "+" over the selected band's whisker bar (so you can grab a handle)
    if (selBand >= 0 && paramCache[(size_t) selBand].on && whiskerRelevant (paramCache[(size_t) selBand].type))
    {
        const auto wk = whiskerEnds (selBand);
        const float ny = nodePos (selBand).y;
        if (std::abs (hoverPos.y - ny) < 12.0f && hoverPos.x > wk.first.x - 8.0f && hoverPos.x < wk.second.x + 8.0f)
            return { -1.0f, -1.0f };
    }

    bool anyFree = false;
    for (int i = 0; i < tabby::kNumBands; ++i) if (! paramCache[(size_t) i].on) { anyFree = true; break; }
    if (! anyFree)                                   return { -1.0f, -1.0f };   // all bands used
    const float cy = dbToY (compositeDb (xToFreq (hoverPos.x)));
    if (std::abs (hoverPos.y - cy) > kAddThreshold)  return { -1.0f, -1.0f };   // not near the curve
    return { hoverPos.x, cy };
}

void EqCurveDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    refreshDesigns();
    const int b = nodeAt (e.position);
    if (b >= 0)                                                   // on a node -> toggle bypass (ghost on/off)
    {
        setParamGestured (tabby::bandId (b, "bypass"), paramCache[(size_t) b].bypass ? 0.0 : 1.0);
        selectBand (b);
    }
    else
        addBandOfType (0, e.position);                            // empty -> add a Bell
}

void EqCurveDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    refreshDesigns();
    const int b = draggingBand >= 0 ? draggingBand : nodeAt (e.position);
    if (b < 0) return;
    auto* qp = proc.apvts.getParameter (tabby::bandId (b, "q"));
    if (qp == nullptr) return;
    const double cur    = qp->convertFrom0to1 (qp->getValue());   // live value (not the timer cache)
    const double factor = wheel.deltaY > 0.0f ? 1.15 : 1.0 / 1.15;
    setParamGestured (tabby::bandId (b, "q"), juce::jlimit (0.05, 40.0, cur * factor));
}
