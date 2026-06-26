// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/EqCurveDisplay.h"

namespace
{
    bool hasGain (teq::FilterType t) noexcept
    {
        return t == teq::FilterType::Bell || t == teq::FilterType::LowShelf || t == teq::FilterType::HighShelf;
    }

    juce::Colour bandColour (teq::FilterType t) noexcept
    {
        using FT = teq::FilterType;
        switch (t)
        {
            case FT::LowShelf:  case FT::HighShelf: return juce::Colour (0xff7fc8ff);
            case FT::HighPass:  case FT::LowPass:   return juce::Colour (0xffff9d5c);
            case FT::BandPass:                      return juce::Colour (0xffb98cff);
            case FT::Bell:      default:            return juce::Colour (0xff6ee7a8);
        }
    }
}

EqCurveDisplay::EqCurveDisplay (TabbyEqAudioProcessor& p) : proc (p)
{
    for (int i = 0; i < (int) window.size(); ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (window.size() - 1));

    specDb.fill (-120.0f);
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
        if (paramCache[b].on)
        {
            h *= teq::evalCoeffs (designCache[b].c0, w);
            if (designCache[b].twoStage) h *= teq::evalCoeffs (designCache[b].c1, w);
        }
    return 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (h)));
}

juce::Point<float> EqCurveDisplay::nodePos (int b) const noexcept
{
    // bells/shelves sit at their OWN gain (so a drag tracks the cursor exactly); HP/LP/BP have no
    // gain, so they ride the composite curve at their corner frequency.
    const double db = hasGain (paramCache[b].type) ? paramCache[b].gainDb : compositeDb (paramCache[b].freq);
    return { freqToX (paramCache[b].freq), dbToY (db) };
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
        if (auto* fp = proc.apvts.getParameter (tabby::bandId (draggingBand, "freq"))) fp->endChangeGesture();
        if (draggingGain)
            if (auto* gp = proc.apvts.getParameter (tabby::bandId (draggingBand, "gain"))) gp->endChangeGesture();
    }
    draggingBand = -1;
    draggingGain = false;
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
    if (p.type != teq::FilterType::LowShelf && p.type != teq::FilterType::HighShelf)   // shelves ignore Q
        s << "   Q " << juce::String (p.Q, 2);
    return s;
}

//==============================================================================
void EqCurveDisplay::pushSpectrum()
{
    if (! proc.pullSpectrum (false, fftBuf.data()))
    {
        // No new frame this tick is NORMAL (a 2048-pt window arrives at ~23 fps vs the 30 Hz timer).
        // Hold the last spectrum; only fade once genuinely starved (audio stopped ~0.5 s).
        if (starveTicks < 16) ++starveTicks;          // bounded — never overflows over long silence
        if (starveTicks > 15)
            for (auto& v : specDb) v += 0.05f * (-120.0f - v);
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
        specDb[(size_t) i] += 0.25f * ((float) db - specDb[(size_t) i]);   // smooth toward target
    }
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

    g.fillAll (juce::Colour (0xff14171c));

    // --- grid -------------------------------------------------------------
    g.setFont (11.0f);
    for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
    {
        const float x = freqToX (f);
        g.setColour (juce::Colour (0x18ffffff));
        g.drawVerticalLine ((int) x, 0.0f, h);
        g.setColour (juce::Colour (0x55ffffff));
        g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, (f == 1000.0 ? 0 : 0)) + "k" : juce::String ((int) f),
                    (int) x + 2, (int) h - 14, 34, 12, juce::Justification::left);
    }
    for (double db : { -18.0, -12.0, -6.0, 0.0, 6.0, 12.0, 18.0 })
    {
        const float y = dbToY (db);
        g.setColour (db == 0.0 ? juce::Colour (0x40ffffff) : juce::Colour (0x14ffffff));
        g.drawHorizontalLine ((int) y, 0.0f, w);
    }

    // --- spectrum (filled) ------------------------------------------------
    {
        juce::Path s;
        bool started = false;
        for (int i = 1; i < (int) specDb.size(); ++i)
        {
            const double f = (double) i * fsCache / (double) teq::kSpectrumFftSize;
            if (f < kFreqMin || f > kFreqMax) continue;
            const float x = freqToX (f), y = specDbToY (specDb[(size_t) i]);
            if (! started) { s.startNewSubPath (x, h); s.lineTo (x, y); started = true; }
            else            s.lineTo (x, y);
        }
        if (started) { s.lineTo (w, h); s.closeSubPath(); g.setColour (juce::Colour (0x2266e7a8)); g.fillPath (s); }
    }

    // --- response curve (sampled on a ~240-pt grid, not per-pixel) --------
    {
        constexpr int pts = 240;
        juce::Path c;
        for (int i = 0; i <= pts; ++i)
        {
            const float x = (float) i / (float) pts * w;
            const float y = dbToY (compositeDb (xToFreq (x)));
            if (i == 0) c.startNewSubPath (x, y); else c.lineTo (x, y);
        }
        g.setColour (juce::Colour (0xffe8eef5));
        g.strokePath (c, juce::PathStrokeType (1.6f));
    }

    // --- nodes ------------------------------------------------------------
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (paramCache[b].on)
        {
            const auto pos = nodePos (b);
            const auto col = bandColour (paramCache[b].type);
            g.setColour (col.withAlpha (0.25f)); g.fillEllipse (pos.x - kNodeR - 2, pos.y - kNodeR - 2, (kNodeR + 2) * 2, (kNodeR + 2) * 2);
            g.setColour (col);                   g.fillEllipse (pos.x - kNodeR,     pos.y - kNodeR,     kNodeR * 2,       kNodeR * 2);
            g.setColour (juce::Colours::black.withAlpha (0.5f)); g.drawEllipse (pos.x - kNodeR, pos.y - kNodeR, kNodeR * 2, kNodeR * 2, 1.0f);
            g.setColour (juce::Colours::white);
            g.setFont (10.0f);
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (pos.x - kNodeR, pos.y - kNodeR, kNodeR * 2, kNodeR * 2), juce::Justification::centred);
        }

    // --- selection highlight + live value bubble --------------------------
    const int rb = draggingBand >= 0 ? draggingBand : selBand;
    if (rb >= 0 && rb < tabby::kNumBands && paramCache[(size_t) rb].on)
    {
        const auto pos = nodePos (rb);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawEllipse (pos.x - kNodeR - 3, pos.y - kNodeR - 3, (kNodeR + 3) * 2, (kNodeR + 3) * 2, 1.5f);

        const juce::String txt = readoutText (rb);
        const float tw = (float) txt.length() * 6.6f + 12.0f, th = 18.0f;
        const float bx = juce::jlimit (2.0f, w - tw - 2.0f, pos.x - tw * 0.5f);
        const float by = juce::jlimit (2.0f, h - th - 2.0f, pos.y - kNodeR - 8.0f - th);
        g.setColour (juce::Colour (0xee0e1014)); g.fillRoundedRectangle (bx, by, tw, th, 4.0f);
        g.setColour (juce::Colour (0x40ffffff)); g.drawRoundedRectangle (bx, by, tw, th, 4.0f, 1.0f);
        g.setColour (juce::Colours::white); g.setFont (12.0f);
        g.drawText (txt, juce::Rectangle<float> (bx, by, tw, th), juce::Justification::centred);
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
        const char* names[] = { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Band Pass" };
        juce::Component::SafePointer<EqCurveDisplay> safe (this);
        juce::PopupMenu m;

        if (b >= 0)   // on a node: change type / remove
        {
            for (int i = 0; i < 6; ++i)
                m.addItem (i + 1, names[i], true, paramCache[b].type == tabby::filterTypeFromChoice (i));
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
                for (int i = 0; i < 6; ++i) add.addItem (i + 1, names[i]);
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
    setParam (tabby::bandId (draggingBand, "freq"), xToFreq (e.position.x));
    if (draggingGain)   // latched at mouseDown so the gain begin/end gesture stays balanced
        setParam (tabby::bandId (draggingBand, "gain"), juce::jlimit (-kGainRange, kGainRange, yToDb (e.position.y)));
}

void EqCurveDisplay::mouseUp (const juce::MouseEvent&) { endDragGesture(); }

void EqCurveDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    refreshDesigns();
    if (nodeAt (e.position) < 0) addBandOfType (0, e.position);   // empty → add a Bell
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
