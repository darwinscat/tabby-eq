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
        return t == teq::FilterType::Bell      || t == teq::FilterType::BandPass
            || t == teq::FilterType::Notch     || t == teq::FilterType::AllPass
            || t == teq::FilterType::LowShelf  || t == teq::FilterType::HighShelf;   // resonant shelves have Q
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

    // Mid/main vs Side param id: "freq" -> "sFreq", "q" -> "sQ", "gain" -> "sGain", "type" -> "sType", ...
    juce::String laneParamId (int b, bool side, const juce::String& base)
    {
        if (side) return tabby::bandId (b, "s" + base.substring (0, 1).toUpperCase() + base.substring (1));
        return tabby::bandId (b, base);
    }

    // HP/LP whisker maps to the DISCRETE slope list (steeper = narrower handle) instead of Q.
    constexpr int kSlopeDb[7] = { 6, 12, 24, 36, 48, 72, 96 };
    int    slopeIndexFromDb (int db) noexcept { for (int i = 0; i < 7; ++i) if (kSlopeDb[i] == db) return i; return 1; }
    double slopeBwForIndex  (int i)  noexcept { return kQwBwHi + (kQwBwLo - kQwBwHi) * (i / 6.0); }   // i0 wide -> i6 narrow
    int    slopeIndexForBw  (double bw) noexcept
    {
        const double t = juce::jlimit (0.0, 1.0, (kQwBwHi - bw) / (kQwBwHi - kQwBwLo));
        return juce::jlimit (0, 6, (int) std::round (t * 6.0));
    }

    juce::Colour typeColour (teq::FilterType t) noexcept
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

    // Hand-picked per-band palette — vibrant, distinct on the dark bg, warm/cool alternating so
    // adjacent band slots read apart. One colour per band.
    const juce::uint32 kBandColours[tabby::kNumBands] = {
        // cool / non-orange first so early bands don't clash with the orange composite line
        0xff5ec8ff, 0xff62d2a2, 0xffb388ff, 0xfff06292, 0xff4dd0e1, 0xff81c784,
        0xff7986cb, 0xffba68c8, 0xff64b5f6, 0xff4db6ac, 0xfff48fb1, 0xff9575cd,
        0xffaed581, 0xff4fc3f7, 0xff80cbc4, 0xff9ccc65,
        // warmer / orange-ish last
        0xffe57373, 0xffdce775, 0xffff8a80, 0xffffd54f, 0xffffb74d, 0xffff8a65,
        0xffffab40, 0xffffd180
    };
}

EqCurveDisplay::EqCurveDisplay (TabbyEqAudioProcessor& p) : proc (p)
{
    for (int i = 0; i < (int) window.size(); ++i)
        window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (window.size() - 1));

    fft.prepare (teq::kSpectrumFftSize);     // core::Fft seam — plan/alloc here (UI thread), not in the timer

    specDb.fill (-120.0f);
    specPeak.fill (-120.0f);
    proc.setAnalyzerActive (true);
    setWantsKeyboardFocus (true);     // so Esc can cancel an in-progress add-drag
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
        designCacheSide[b] = (paramCache[b].on && paramCache[b].ms) ? teq::designBand (sideView (b), fsCache) : teq::BandDesign {};
    }
}

teq::BandParams EqCurveDisplay::sideView (int b) const noexcept   // the Side lane as a plain BandParams
{
    teq::BandParams v = paramCache[(size_t) b];
    v.type = v.sType; v.freq = v.sFreq; v.Q = v.sQ; v.gainDb = v.sGainDb; v.slope = v.sSlope;
    v.on = v.sOn; v.bypass = v.sBypass; v.swept = false; v.ms = false;
    return v;
}

double EqCurveDisplay::compositeDb (double f, bool side) const noexcept
{
    const double w = 2.0 * juce::MathConstants<double>::pi * f / fsCache;
    std::complex<double> h { 1.0, 0.0 };
    for (int b = 0; b < tabby::kNumBands; ++b)
    {
        if (! paramCache[b].on) continue;
        if (paramCache[b].ms && side)   // Side composite: an M/S band contributes its Side lane
        {
            if (paramCache[b].sOn && ! paramCache[b].sBypass)
                for (int s = 0; s < designCacheSide[b].n; ++s) h *= teq::evalCoeffs (designCacheSide[b].sec[s], w);
        }
        else                            // Mid composite, or a Stereo band (which affects both M and S)
        {
            if (! paramCache[b].bypass)
                for (int s = 0; s < designCache[b].n; ++s) h *= teq::evalCoeffs (designCache[b].sec[s], w);
        }
    }
    return 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (h)));
}

juce::Point<float> EqCurveDisplay::nodePos (int b, bool side) const noexcept
{
    // bells/shelves/tilt sit at their OWN gain (a drag tracks the cursor); notch/all-pass are
    // surgical / phase-only so they sit on the 0 dB line; HP/LP/BP ride the composite at the corner.
    const auto& bp = paramCache[(size_t) b];
    const bool   S = bp.ms && side;
    const auto   t = S ? bp.sType : bp.type;
    const double f = S ? bp.sFreq : bp.freq;
    const double g = S ? bp.sGainDb : bp.gainDb;
    double db;
    if (hasGain (t))                                                       db = g;
    else if (t == teq::FilterType::Notch || t == teq::FilterType::AllPass) db = 0.0;
    else                                                                   db = compositeDb (f, S);
    return { freqToX (f), dbToY (db) };
}

std::pair<juce::Point<float>, juce::Point<float>> EqCurveDisplay::whiskerEnds (int b, bool side) const noexcept
{
    const auto& bp = paramCache[(size_t) b];
    const bool   S = bp.ms && side;
    const auto  pos = nodePos (b, side);
    const double f0 = S ? bp.sFreq : bp.freq;
    const auto   t  = S ? bp.sType : bp.type;
    const int    sl = S ? bp.sSlope : bp.slope;
    const double qv = S ? bp.sQ : bp.Q;
    const double bw = isCut (t) ? slopeBwForIndex (slopeIndexFromDb (sl)) : whiskerBwForQ (qv);   // half-bandwidth (oct)
    const float dx  = freqToX (f0 * std::exp2 (bw)) - pos.x;                  // -> px offset (log-x, symmetric)
    return { { pos.x - dx, pos.y }, { pos.x + dx, pos.y } };
}

juce::Colour EqCurveDisplay::bandColour (int b) const noexcept
{
    if (perBandColors)   // hand-picked fixed colour per band slot
        return juce::Colour (kBandColours[juce::jlimit (0, tabby::kNumBands - 1, b)]);
    return typeColour (paramCache[(size_t) b].type);
}

double EqCurveDisplay::bandDb (int b, double f, bool side) const noexcept
{
    const auto& d = (paramCache[(size_t) b].ms && side) ? designCacheSide[(size_t) b] : designCache[(size_t) b];
    const double w = 2.0 * juce::MathConstants<double>::pi * f / fsCache;
    std::complex<double> h { 1.0, 0.0 };
    for (int s = 0; s < d.n; ++s) h *= teq::evalCoeffs (d.sec[s], w);
    return 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (h)));
}

EqCurveDisplay::Hit EqCurveDisplay::nodeAt (juce::Point<float> p) const noexcept
{
    Hit best; float bestD = (kNodeR + 5.0f) * (kNodeR + 5.0f);
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (paramCache[b].on)
        {
            const float dm = p.getDistanceSquaredFrom (nodePos (b, false));
            if (dm < bestD) { bestD = dm; best = { b, false }; }
            if (paramCache[b].ms)
            {
                const float ds = p.getDistanceSquaredFrom (nodePos (b, true));
                if (ds < bestD) { bestD = ds; best = { b, true }; }
            }
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

void EqCurveDisplay::driveAudition (bool on, float freqHz, float q)
{
    auditioning = on;
    if (on) { audFreq = freqHz; audQ = q; }
    proc.setAudition (on, freqHz, q);
    repaint();
}

void EqCurveDisplay::endDragGesture()
{
    driveAudition (false);   // never leave the drag-audition latched
    if (draggingBand >= 0)
    {
        if (draggingQ)
        {
            if (auto* prm = proc.apvts.getParameter (laneParamId (draggingBand, draggingSide, whiskerSlope ? "slope" : "q")))
                prm->endChangeGesture();
        }
        else
        {
            if (auto* fp = proc.apvts.getParameter (laneParamId (draggingBand, draggingSide, "freq"))) fp->endChangeGesture();
            if (draggingGain)
                if (auto* gp = proc.apvts.getParameter (laneParamId (draggingBand, draggingSide, "gain"))) gp->endChangeGesture();
        }
    }
    if (momentarySolo) { proc.setSoloBand (prevSoloBand); momentarySolo = false; }   // release momentary solo
    pressBand    = -1;
    pressMoved   = false;
    draggingBand = -1;
    draggingSide = false;
    draggingGain = false;
    draggingQ    = false;
    qDragSide    = 0;
    whiskerSlope = false;
}

void EqCurveDisplay::selectBand (int newSel, bool side)
{
    if (newSel == selBand && side == selSide) return;
    selBand = newSel;
    selSide = (newSel >= 0) && side;
    if (onBandSelected) onBandSelected (selBand, selSide);   // updates the toolbar's controls + lane first
    positionToolbar();                                       // then place it near the new node
}

void EqCurveDisplay::setSelectedSide (bool side)             // the toolbar switched Mid/Side lane
{
    if (side == selSide) return;
    selSide = side;
    positionToolbar();
    repaint();
}

void EqCurveDisplay::setToolbar (juce::Component* t) noexcept
{
    toolbar = t;
    if (t != nullptr) addChildComponent (t);         // hidden until a band is selected
}

// 0 — Classic: the original. Centre the strip on the node, above it (below if it'd clip the top), clamp.
juce::Rectangle<int> EqCurveDisplay::placeClassic (juce::Point<float> node) const noexcept
{
    int tx = (int) (node.x - kToolbarW * 0.5f);
    int ty = (int) (node.y - kNodeR - 12.0f - kToolbarH);            // above the node...
    if (ty < 2) ty = (int) (node.y + kNodeR + 12.0f);               // ...or below if no room
    tx = juce::jlimit (2, juce::jmax (2, getWidth()  - kToolbarW - 2), tx);
    ty = juce::jlimit (2, juce::jmax (2, getHeight() - kToolbarH - 2), ty);
    return { tx, ty, kToolbarW, kToolbarH };
}

// 1 — Anchor-to-open-side: don't straddle the node. Sit it just inside one horizontal end and extend into the
// larger canvas gap, so it covers (at most) the neighbours on the emptier side instead of both sides at once.
juce::Rectangle<int> EqCurveDisplay::placeAnchorSide (juce::Point<float> node) const noexcept
{
    const bool right = node.x < (float) getWidth() * 0.5f;           // more room to the right → extend right
    int tx = right ? (int) (node.x - kToolbarW * 0.12f)
                   : (int) (node.x - kToolbarW * 0.88f);
    int ty = (int) (node.y - kNodeR - 12.0f - kToolbarH);
    if (ty < 2) ty = (int) (node.y + kNodeR + 12.0f);
    tx = juce::jlimit (2, juce::jmax (2, getWidth()  - kToolbarW - 2), tx);
    ty = juce::jlimit (2, juce::jmax (2, getHeight() - kToolbarH - 2), ty);
    return { tx, ty, kToolbarW, kToolbarH };
}

// 2/3 core — score 8 candidate slots around the node (above/below/left/right + diagonals) by how many OTHER
// band nodes they'd cover, then off-canvas overhang, then closeness; a light bonus keeps the last slot (no
// flicker while dragging). Returns the clamped winner + its post-clamp occlusion + slot index.
juce::Rectangle<int> EqCurveDisplay::bestFloatCandidate (juce::Point<float> node, int& occlOut, int& slotOut) const noexcept
{
    const float gap = kNodeR + 12.0f, W = (float) kToolbarW, H = (float) kToolbarH;
    const juce::Point<float> cand[8] = {
        { node.x - W * 0.5f,  node.y - gap - H },   // 0 above-center
        { node.x - W * 0.5f,  node.y + gap     },   // 1 below-center
        { node.x + gap,       node.y - H * 0.5f },  // 2 right
        { node.x - gap - W,   node.y - H * 0.5f },  // 3 left
        { node.x + gap,       node.y - gap - H },   // 4 above-right
        { node.x - gap - W,   node.y - gap - H },   // 5 above-left
        { node.x + gap,       node.y + gap     },   // 6 below-right
        { node.x - gap - W,   node.y + gap     },   // 7 below-left
    };

    juce::Point<float> others[tabby::kNumBands * 2]; int no = 0;     // every OTHER visible node centre
    for (int b = 0; b < tabby::kNumBands; ++b) if (paramCache[b].on)
    {
        if (! (b == selBand && ! selSide))              others[no++] = nodePos (b, false);
        if (paramCache[b].ms && ! (b == selBand && selSide)) others[no++] = nodePos (b, true);
    }

    const juce::Rectangle<float> canvas (0.0f, 0.0f, (float) getWidth(), (float) getHeight());
    int best = 0; float bestScore = 1.0e30f;
    for (int s = 0; s < 8; ++s)
    {
        const juce::Rectangle<float> r (cand[s].x, cand[s].y, W, H);
        int occl = 0; for (int i = 0; i < no; ++i) if (r.contains (others[i])) ++occl;
        const float over = juce::jmax (0.0f, canvas.getX() - r.getX())
                         + juce::jmax (0.0f, r.getRight()  - canvas.getRight())
                         + juce::jmax (0.0f, canvas.getY() - r.getY())
                         + juce::jmax (0.0f, r.getBottom() - canvas.getBottom());
        float score = (float) occl * 1000.0f + over * 20.0f + r.getCentre().getDistanceFrom (node) * 0.05f;
        if (s == lastPlaceSlot) score -= 40.0f;                     // hysteresis: prefer the last slot
        if (score < bestScore) { bestScore = score; best = s; }
    }
    slotOut = best;
    const int tx = juce::jlimit (2, juce::jmax (2, getWidth()  - kToolbarW - 2), (int) cand[best].x);
    const int ty = juce::jlimit (2, juce::jmax (2, getHeight() - kToolbarH - 2), (int) cand[best].y);
    const juce::Rectangle<float> rc ((float) tx, (float) ty, W, H);
    occlOut = 0; for (int i = 0; i < no; ++i) if (rc.contains (others[i])) ++occlOut;
    return { tx, ty, kToolbarW, kToolbarH };
}

void EqCurveDisplay::positionToolbar()
{
    if (toolbar == nullptr) return;
    refreshDesigns();   // ensure the cache reflects a just-added/just-toggled band before we test it
    const bool show = selBand >= 0 && selBand < tabby::kNumBands && paramCache[(size_t) selBand].on;
    if (! show) { toolbar->setVisible (false); showLeader = false; return; }

    const auto node = nodePos (selBand, selSide);   // lane-aware (M/S: place at the actual selected node)
    juce::Rectangle<int> b;
    showLeader = false;

    switch (toolbarPlace)
    {
        case ToolbarPlace::AnchorSide:
            b = placeAnchorSide (node);
            break;

        case ToolbarPlace::Collision:
        {
            int occl = 0, slot = 0;
            b = bestFloatCandidate (node, occl, slot);
            lastPlaceSlot = slot;
            showLeader = (slot >= 2);   // 0/1 hug the node (above/below); 2-7 are offset → draw a connector
            break;
        }

        case ToolbarPlace::Hybrid:
        {
            int occl = 0, slot = 0;
            b = bestFloatCandidate (node, occl, slot);
            if (occl > 0)   // no clean local slot → dock to the far horizontal edge, leader across
            {
                const bool dockTop = node.y > (float) getHeight() * 0.5f;
                const int tx = juce::jlimit (2, juce::jmax (2, getWidth() - kToolbarW - 2), (int) (node.x - kToolbarW * 0.5f));
                const int ty = dockTop ? 2 : juce::jmax (2, getHeight() - kToolbarH - 2);
                b = { tx, ty, kToolbarW, kToolbarH };
                lastPlaceSlot = -2;   // docked
                showLeader = true;
            }
            else { lastPlaceSlot = slot; showLeader = (slot >= 2); }
            break;
        }

        case ToolbarPlace::Classic:
        default:
            b = placeClassic (node);
            break;
    }

    if (showLeader) { leaderNode = node; leaderBar = b.toFloat().getCentre(); }

    if (toolbar->getBounds() != b) toolbar->setBounds (b);
    if (! toolbar->isVisible()) toolbar->setVisible (true);
    toolbar->toFront (false);
}

void EqCurveDisplay::resized() { positionToolbar(); }

void EqCurveDisplay::clearSelection() { selectBand (-1); }

void EqCurveDisplay::refreshToolbar() { positionToolbar(); }   // re-place after a window-slider edit ends

void EqCurveDisplay::stepSelection (int dir)
{
    refreshDesigns();
    int idx[tabby::kNumBands]; int n = 0;
    for (int b = 0; b < tabby::kNumBands; ++b) if (paramCache[b].on) idx[n++] = b;
    if (n == 0) return;
    for (int i = 1; i < n; ++i)                       // insertion sort by frequency (n <= 24)
    {
        const int k = idx[i]; int j = i - 1;
        while (j >= 0 && paramCache[idx[j]].freq > paramCache[k].freq) { idx[j + 1] = idx[j]; --j; }
        idx[j + 1] = k;
    }
    int cur = -1;
    for (int i = 0; i < n; ++i) if (idx[i] == selBand) { cur = i; break; }
    const int next = (cur < 0) ? (dir > 0 ? 0 : n - 1) : (((cur + dir) % n) + n) % n;
    selectBand (idx[next]);
}

juce::String EqCurveDisplay::readoutText (int b) const
{
    const auto& p = paramCache[(size_t) b];
    const double f = p.freq;
    juce::String s = f >= 1000.0 ? juce::String (f / 1000.0, 2) + " kHz"
                                 : juce::String (juce::roundToInt (f)) + " Hz";
    if (hasGain (p.type))
        s << "   " << (p.gainDb >= 0.0 ? "+" : "") << juce::String (p.gainDb, 1) << " dB";
    const bool showsQ = p.type != teq::FilterType::Tilt;                                 // tilt ignores Q (shelves now resonant)
    if (showsQ)
        s << "   Q " << juce::String (p.Q, 1);
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
    fft.forward (fftBuf.data(), spec.data());                    // real[N] -> spectrum [DC, Nyquist, re1,im1, …]

    const double norm = (double) teq::kSpectrumFftSize * 0.25;   // Hann single-bin compensation
    const int    N    = teq::kSpectrumFftSize;
    for (int i = 0; i < (int) specDb.size(); ++i)
    {
        const float re  = (i == 0) ? spec[0] : (i == N / 2) ? spec[1] : spec[(size_t) (2 * i)];
        const float im  = (i == 0 || i == N / 2) ? 0.0f : spec[(size_t) (2 * i + 1)];
        const float mag = std::sqrt (re * re + im * im);         // |bin| of the unnormalized forward (== JUCE)
        const double db = juce::Decibels::gainToDecibels ((double) mag / norm, -120.0);
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
    // hold a node still for ~0.35 s -> momentary solo while the mouse stays down
    if (longPressSolo && pressBand >= 0 && ! pressMoved && ! momentarySolo
        && juce::Time::getMillisecondCounter() - pressMs > 350)
    {
        momentarySolo = true;
        prevSoloBand  = proc.getSoloBand();
        proc.setSoloBand (pressBand);
    }
    pushSpectrum();
    repaint();
}

// Shared "listen" overlay: a tall narrow bell OR a spotlight (per the View option), plus the orange
// centre beam + label at f0. Used by both drag-audition and solo so the two gestures look identical.
void EqCurveDisplay::drawListenVisual (juce::Graphics& g, double f0c, double qc, const juce::String& label) const
{
    const float  w = (float) getWidth(), h = (float) getHeight();
    const double f0 = juce::jlimit (kFreqMin, kFreqMax, f0c);
    const float  xc = freqToX (f0);
    const auto   orng = tabby::palette::orange();

    if (audVisual == AudVisual::Bell)
    {
        teq::BandParams bp;
        bp.on = true; bp.type = teq::FilterType::Bell; bp.freq = f0;
        bp.Q = juce::jmax (0.5, qc); bp.gainDb = 20.0;                       // a tall narrow bell = what you hear
        const float y0 = dbToY (0.0);
        juce::Path line, fill; bool started = false;
        for (float x = 0.0f; x <= w; x += 3.0f)
        {
            const double wd = 2.0 * juce::MathConstants<double>::pi * xToFreq (x) / fsCache;
            const double db = 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (teq::bandResponse (bp, fsCache, wd))));
            const float  y  = dbToY (db);
            if (! started) { line.startNewSubPath (x, y); fill.startNewSubPath (x, y0); fill.lineTo (x, y); started = true; }
            else           { line.lineTo (x, y); fill.lineTo (x, y); }
        }
        fill.lineTo (w, y0); fill.closeSubPath();
        g.setColour (orng.withAlpha (0.14f)); g.fillPath (fill);
        g.setColour (orng.withAlpha (0.90f)); g.strokePath (line, juce::PathStrokeType (1.6f));
    }
    else   // Spotlight: dim everything outside the narrow listen band
    {
        const double inv = 1.0 / (2.0 * juce::jmax (0.5, qc));               // -3 dB band-pass edges (octave-ish)
        const double k   = std::sqrt (1.0 + inv * inv);
        const float  xLo = freqToX (f0 * (k - inv));
        const float  xHi = freqToX (f0 * (k + inv));
        g.setColour (juce::Colours::black.withAlpha (0.5f));
        g.fillRect (0.0f, 0.0f, juce::jmax (0.0f, xLo), h);
        g.fillRect (xHi, 0.0f, juce::jmax (0.0f, w - xHi), h);
        g.setColour (orng.withAlpha (0.10f));
        g.fillRect (xLo, 0.0f, juce::jmax (0.0f, xHi - xLo), h);
    }

    g.setColour (orng.withAlpha (0.85f));                                    // centre beam + label (both modes)
    g.drawLine (xc, 0.0f, xc, h, 1.5f);
    const juce::String fl = f0 >= 1000.0 ? juce::String (f0 / 1000.0, 2) + " kHz"
                                         : juce::String (juce::roundToInt (f0)) + " Hz";
    g.setColour (tabby::palette::text());
    g.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Bold")));
    g.drawText (label + "  " + fl, juce::Rectangle<float> (juce::jmin (xc + 6.0f, w - 116.0f), 4.0f, 116.0f, 14.0f),
                juce::Justification::centredLeft);
}

//==============================================================================
void EqCurveDisplay::paint (juce::Graphics& g)
{
    const auto w = (float) getWidth(), h = (float) getHeight();
    refreshDesigns();
    const int solo = proc.getSoloBand();   // >=0: spotlight that band's curve, dim the composite

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

    // --- per-band response curves (colour line + subtle fill); soloing -> only that band; the
    //     selected band's own curve is lifted ------------------------------------------------------
    if (perBandCurves || solo >= 0)
    {
        constexpr int pts = 240;
        const float y0 = dbToY (0.0);
        auto drawLane = [&] (int b, bool side)
        {
            const bool laneByp = side ? (paramCache[b].sBypass || ! paramCache[b].sOn) : paramCache[b].bypass;
            if (laneByp) return;
            juce::Path bc, bf;
            bf.startNewSubPath (0.0f, y0);
            for (int i = 0; i <= pts; ++i)
            {
                const float x = (float) i / (float) pts * w;
                const float y = dbToY (bandDb (b, xToFreq (x), side));
                if (i == 0) bc.startNewSubPath (x, y); else bc.lineTo (x, y);
                bf.lineTo (x, y);
            }
            bf.lineTo (w, y0); bf.closeSubPath();

            const bool hot = (solo == b) || (solo < 0 && b == selBand && side == selSide);
            const auto col = bandColour (b);
            if (perBandFill) { g.setColour (col.withAlpha (hot ? 0.12f : 0.05f)); g.fillPath (bf); }
            g.setColour (col.withAlpha (hot ? 0.70f : 0.32f)); g.strokePath (bc, juce::PathStrokeType (hot ? 1.4f : 0.9f));
        };
        for (int b = 0; b < tabby::kNumBands; ++b)
            if (paramCache[b].on && (solo < 0 || solo == b))
            {
                drawLane (b, false);
                if (paramCache[b].ms) drawLane (b, true);
            }
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
        const bool dim = (solo >= 0);   // soloing: pull the composite right back so the band stands out
        const auto  vio   = tabby::palette::violet();       // fill (both halves): brand violet, gradient
        const float aZero = dim ? 0.06f : 0.40f;            // densest at the 0 dB line; fades to 0 toward +-24 dB
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (0, 0, (int) w, juce::jmax (0, (int) y0));
            g.setGradientFill (juce::ColourGradient (vio.withAlpha (0.0f),  0.0f, 0.0f,
                                                     vio.withAlpha (aZero), 0.0f, y0, false));
            g.fillPath (fill);
        }
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (0, (int) y0, (int) w, juce::jmax (0, (int) (h - y0)));
            g.setGradientFill (juce::ColourGradient (vio.withAlpha (0.0f),  0.0f, h,
                                                     vio.withAlpha (aZero), 0.0f, y0, false));
            g.fillPath (fill);
        }

        // composite curve = brand orange, with a faint orange glow (stacked strokes, no real blur)
        const auto orng = tabby::palette::orange();
        if (! dim)
        {
            g.setColour (orng.withAlpha (0.12f)); g.strokePath (line, juce::PathStrokeType (5.0f));
            g.setColour (orng.withAlpha (0.22f)); g.strokePath (line, juce::PathStrokeType (2.5f));
        }
        g.setColour (dim ? orng.withAlpha (0.30f) : orng);
        g.strokePath (line, juce::PathStrokeType (1.8f));

        // Side composite (when any band is M/S): a second curve in light violet. The orange line above
        // is then the Mid composite; together they show what L/R become.
        bool anyMs = false;
        for (int b = 0; b < tabby::kNumBands; ++b) if (paramCache[b].on && paramCache[b].ms) { anyMs = true; break; }
        if (anyMs)
        {
            juce::Path sline; bool st = false;
            for (int i = 0; i <= pts; ++i)
            {
                const float x = (float) i / (float) pts * w;
                const float y = dbToY (compositeDb (xToFreq (x), true));
                if (! st) { sline.startNewSubPath (x, y); st = true; } else sline.lineTo (x, y);
            }
            g.setColour (tabby::palette::violetLo().withAlpha (dim ? 0.40f : 0.95f));
            g.strokePath (sline, juce::PathStrokeType (1.6f));
        }
    }

    // --- hover halo on the node under the cursor --------------------------
    if (hoverPos.x >= 0.0f && draggingBand < 0)
    {
        const Hit hb = nodeAt (hoverPos);
        if (hb.band >= 0)
        {
            const auto hp = nodePos (hb.band, hb.side);
            g.setColour (bandColour (hb.band).withAlpha (0.22f));
            g.fillEllipse (hp.x - kNodeR - 7, hp.y - kNodeR - 7, (kNodeR + 7) * 2, (kNodeR + 7) * 2);
        }
    }

    // --- toolbar leader line (Collision / Hybrid placement): a thin connector node -> strip -----------
    if (showLeader && toolbar != nullptr && toolbar->isVisible())
    {
        g.setColour (tabby::palette::violetLo().withAlpha (0.50f));
        g.drawLine (leaderNode.x, leaderNode.y, leaderBar.x, leaderBar.y, 1.4f);
        g.setColour (tabby::palette::violetLo().withAlpha (0.90f));
        g.fillEllipse (leaderNode.x - 2.5f, leaderNode.y - 2.5f, 5.0f, 5.0f);
    }

    // --- nodes (Mid/main always; Side too for M/S bands) ------------------
    auto drawNode = [&] (int b, bool side)
    {
        const auto pos = nodePos (b, side);
        const auto col = bandColour (b);
        const auto numR = juce::Rectangle<float> (pos.x - kNodeR, pos.y - kNodeR, kNodeR * 2, kNodeR * 2);
        const bool byp = side ? (paramCache[b].sBypass || ! paramCache[b].sOn) : paramCache[b].bypass;
        if (byp)   // ghost: dim hollow ring — still hit-tested, so it's clickable back on
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
        if (side)   // Side badge: an orange ring marks the Side node
        {
            g.setColour (tabby::palette::orange().withAlpha (0.95f));
            g.drawEllipse (pos.x - kNodeR - 3.0f, pos.y - kNodeR - 3.0f, (kNodeR + 3.0f) * 2, (kNodeR + 3.0f) * 2, 1.6f);
        }
    };
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (paramCache[b].on)
        {
            drawNode (b, false);
            if (paramCache[b].ms) drawNode (b, true);
        }

    // --- selection highlight (the focused lane's node) --------------------
    const int  rb    = draggingBand >= 0 ? draggingBand : selBand;
    const bool rside = draggingBand >= 0 ? draggingSide : selSide;
    if (rb >= 0 && rb < tabby::kNumBands && paramCache[(size_t) rb].on)
    {
        const auto pos = nodePos (rb, rside);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawEllipse (pos.x - kNodeR - 3, pos.y - kNodeR - 3, (kNodeR + 3) * 2, (kNodeR + 3) * 2, 1.5f);

        // Whiskers (Neutron-style): drag an end handle to set bandwidth/Q (HP/LP -> discrete slope)
        const auto laneType = (paramCache[(size_t) rb].ms && rside) ? paramCache[(size_t) rb].sType : paramCache[(size_t) rb].type;
        if (whiskerRelevant (laneType))
        {
            const auto wk  = whiskerEnds (rb, rside);
            const auto col = bandColour (rb);
            g.setColour (col.withAlpha (0.85f));
            g.drawLine (wk.first.x, pos.y, wk.second.x, pos.y, 1.5f);
            for (const auto& hp : { wk.first, wk.second })
            {
                g.setColour (tabby::palette::bg()); g.fillEllipse (hp.x - 4.0f, hp.y - 4.0f, 8.0f, 8.0f);
                g.setColour (col);                  g.drawEllipse (hp.x - 4.0f, hp.y - 4.0f, 8.0f, 8.0f, 1.5f);
            }
        }

        // (the live value bubble is gone — the floating toolbar shows freq/Q/gain now)
    }

    // --- "listen" visualisation (shared): bell or spotlight per the View option + the beam.
    //     Drag-audition takes precedence; otherwise a soloed band (incl. long-press momentary). ---
    if (auditioning)
        drawListenVisual (g, (double) audFreq, (double) audQ, "LISTEN");
    else if (solo >= 0 && solo < tabby::kNumBands && paramCache[(size_t) solo].on)
        drawListenVisual (g, paramCache[(size_t) solo].freq, (double) audQSetting, "SOLO");   // width from the Listen-Q config

    // --- add-band affordance: live press-drag preview, or the hovering "+" near a trigger line ---
    if (placing)
    {
        drawAddPreview (g, placeSpec, placePos, placeGainFromDrag);
    }
    else
    {
        const auto addBtn = addButtonAt();
        if (addBtn.x >= 0.0f)
        {
            drawAddPreview (g, predictAdd (addBtn), addBtn, false);   // ghost of what a click adds (default gain)
            const float r = kNodeR + 2.0f;
            g.setColour (tabby::palette::panel().withAlpha (0.92f));    g.fillEllipse (addBtn.x - r, addBtn.y - r, r * 2, r * 2);
            g.setColour (tabby::palette::violetLo().withAlpha (0.95f)); g.drawEllipse (addBtn.x - r, addBtn.y - r, r * 2, r * 2, 1.5f);
            g.setColour (tabby::palette::text());
            g.drawLine (addBtn.x - 4.0f, addBtn.y, addBtn.x + 4.0f, addBtn.y, 1.6f);
            g.drawLine (addBtn.x, addBtn.y - 4.0f, addBtn.x, addBtn.y + 4.0f, 1.6f);
        }
    }
}

//==============================================================================
void EqCurveDisplay::addBandOfType (int typeIndex, juce::Point<float> at, int slopeIndex)
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
            setParamGestured (tabby::bandId (b, "q"), (ft == teq::FilterType::HighPass  || ft == teq::FilterType::LowPass
                                                       || ft == teq::FilterType::LowShelf || ft == teq::FilterType::HighShelf) ? 0.707 : 1.0);
            if (slopeIndex >= 0) setParamGestured (tabby::bandId (b, "slope"), (double) slopeIndex);
            setParamGestured (tabby::bandId (b, "bypass"), 0.0);
            setParamGestured (tabby::bandId (b, "on"), 1.0);
            selectBand (b);
            return;
        }
}

void EqCurveDisplay::drawAddPreview (juce::Graphics& g, const AddSpec& s, juce::Point<float> at, bool dragging) const
{
    const auto ft = tabby::filterTypeFromChoice (s.typeIndex);

    teq::BandParams bp;
    bp.on     = true;
    bp.type   = ft;
    bp.freq   = juce::jlimit (kFreqMin, kFreqMax, xToFreq (at.x));
    bp.Q      = isCut (ft) ? 0.707 : 1.0;
    const double gain = s.hasGainCtrl ? (dragging ? juce::jlimit (-kGainRange, kGainRange, yToDb (at.y)) : s.gainDb) : 0.0;
    bp.gainDb = gain;
    bp.slope  = kSlopeDb[juce::jlimit (0, 6, s.slopeIndex < 0 ? 1 : s.slopeIndex)];

    const auto col = tabby::palette::violetLo();

    // ghost response curve
    juce::Path path;
    const float wpx = (float) getWidth();
    bool started = false;
    for (float x = 0.0f; x <= wpx; x += 3.0f)
    {
        const double wd = 2.0 * juce::MathConstants<double>::pi * xToFreq (x) / fsCache;
        const double db = 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (teq::bandResponse (bp, fsCache, wd))));
        const float  y  = dbToY (db);
        if (! started) { path.startNewSubPath (x, y); started = true; } else path.lineTo (x, y);
    }
    g.setColour (col.withAlpha (0.5f));
    g.strokePath (path, juce::PathStrokeType (1.4f));

    // node dot (matches nodePos: hasGain -> own gain; notch/all-pass -> 0; cut -> its own corner)
    double nodeDb = 0.0;
    if (hasGain (ft)) nodeDb = gain;
    else if (! (ft == teq::FilterType::Notch || ft == teq::FilterType::AllPass))
        nodeDb = 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (
                     teq::bandResponse (bp, fsCache, 2.0 * juce::MathConstants<double>::pi * bp.freq / fsCache))));
    const float nx = freqToX (bp.freq), ny = dbToY (nodeDb);
    g.setColour (col.withAlpha (0.9f));
    g.drawEllipse (nx - kNodeR, ny - kNodeR, kNodeR * 2.0f, kNodeR * 2.0f, 1.5f);

    // type icon + label
    const char* names[] = { "Bell", "Low Shelf", "High Shelf", "HPF", "LPF", "Band Pass", "Notch", "All Pass", "Tilt" };
    juce::String label = names[juce::jlimit (0, 8, s.typeIndex)];
    if (isCut (ft)) label << " " << juce::String (bp.slope);
    label << "   " << (bp.freq >= 1000.0 ? juce::String (bp.freq / 1000.0, 2) + " kHz"
                                         : juce::String (juce::roundToInt (bp.freq)) + " Hz");
    if (! isCut (ft) && s.hasGainCtrl) label << "   " << (gain >= 0.0 ? "+" : "") << juce::String (gain, 1) << " dB";

    const float lblW = (float) label.length() * 6.6f + 22.0f, lblH = 16.0f;
    const float lx = juce::jlimit (2.0f, wpx - lblW - 2.0f, nx + kNodeR + 6.0f);
    const float ly = juce::jlimit (2.0f, (float) getHeight() - lblH - 2.0f, ny - kNodeR - 18.0f);
    g.setColour (tabby::palette::panel().withAlpha (0.92f));
    g.fillRoundedRectangle (lx, ly, lblW, lblH, 4.0f);
    if (auto d = tabby::shapes::icon (ft, col))
        d->drawWithin (g, juce::Rectangle<float> (lx + 2.0f, ly + 2.0f, 14.0f, lblH - 4.0f), juce::RectanglePlacement::centred, 1.0f);
    g.setColour (tabby::palette::text());
    g.setFont (11.0f);
    g.drawText (label, juce::Rectangle<float> (lx + 18.0f, ly, lblW - 20.0f, lblH), juce::Justification::centredLeft);
}

EqCurveDisplay::AddSpec EqCurveDisplay::predictAdd (juce::Point<float> at) const noexcept
{
    const double f      = xToFreq (at.x);
    const bool   above0 = yToDb (at.y) >= 0.0;                                       // side of the 0 dB line
    const double frac   = std::log (f / kFreqMin) / std::log (kFreqMax / kFreqMin);  // 0 (20 Hz) .. 1 (20 kHz)
    const bool   left   = frac < (1.0 / 3.0);                                        // freq thirds (log axis)
    const bool   right  = frac > (2.0 / 3.0);

    bool hasHP = false, hasLP = false, hasLowShelf = false, hasHighShelf = false;
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (paramCache[b].on)
        {
            const auto t = paramCache[b].type;
            hasHP        = hasHP        || t == teq::FilterType::HighPass;
            hasLP        = hasLP        || t == teq::FilterType::LowPass;
            hasLowShelf  = hasLowShelf  || t == teq::FilterType::LowShelf;
            hasHighShelf = hasHighShelf || t == teq::FilterType::HighShelf;
        }

    AddSpec s;
    s.gainDb = above0 ? 2.0 : -2.0;                          // default boost/cut by side of 0 dB

    if (left)
    {
        if (! above0 && ! hasHP) { s.typeIndex = 3; s.slopeIndex = 2; s.gainDb = 0.0; s.hasGainCtrl = false; }  // HPF 24
        else if (! hasLowShelf)  { s.typeIndex = 1; }                                                           // Low Shelf +-2
        else                     { s.typeIndex = 0; }                                                           // Bell +-2
    }
    else if (right)
    {
        if (! above0 && ! hasLP) { s.typeIndex = 4; s.slopeIndex = 1; s.gainDb = 0.0; s.hasGainCtrl = false; }  // LPF 12
        else if (! hasHighShelf) { s.typeIndex = 2; }                                                           // High Shelf +-2
        else                     { s.typeIndex = 0; }                                                           // Bell +-2
    }
    else                         { s.typeIndex = 0; }                                                           // middle -> Bell +-2

    return s;
}

void EqCurveDisplay::smartAdd (juce::Point<float> at)
{
    refreshDesigns();
    const AddSpec s = predictAdd (at);
    if (s.hasGainCtrl) addBandOfType (s.typeIndex, { at.x, dbToY (s.gainDb) }, s.slopeIndex);   // default +-2 by side
    else               addBandOfType (s.typeIndex, at, s.slopeIndex);                            // cut: gain ignored
}

void EqCurveDisplay::mouseDown (const juce::MouseEvent& e)
{
    refreshDesigns();
    const Hit  hit  = nodeAt (e.position);
    const int  b    = hit.band;
    const bool side = hit.side;

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
                it.setTicked ((side ? paramCache[b].sType : paramCache[b].type) == tabby::filterTypeFromChoice (i));
                m.addItem (it);
            }
            m.addSeparator();
            m.addItem (100, "Remove band");
            m.showMenuAsync (juce::PopupMenu::Options(), [safe, b, side] (int r) {
                if (safe == nullptr || r == 0) return;
                if (r == 100) safe->setParamGestured (tabby::bandId (b, "on"), 0.0);   // remove the whole band
                else          safe->setParamGestured (laneParamId (b, side, "type"), r - 1);   // set this lane's type
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

    // "+" near a trigger line -> start a press-drag placement (drag to set gain, release to commit)
    const auto addBtn = addButtonAt();
    if (addBtn.x >= 0.0f)
    {
        placing = true; placeMoved = false; placeGainFromDrag = false; placeDown = e.position; placePos = e.position;
        placeSpec = predictAdd (e.position);
        lastDragFreq = (float) xToFreq (e.position.x);
        grabKeyboardFocus();   // so Esc can cancel
        if (e.mods.isAltDown()) driveAudition (true, lastDragFreq, audQSetting);   // engage at once if Alt already held
        repaint();
        return;
    }

    // A drag on the selected lane's Q-whisker handle sets Q (Neutron-style), keeping the selection.
    const auto selType = (selBand < 0) ? teq::FilterType::Bell
                       : ((paramCache[(size_t) selBand].ms && selSide) ? paramCache[(size_t) selBand].sType
                                                                       : paramCache[(size_t) selBand].type);
    if (selBand >= 0 && b < 0 && paramCache[(size_t) selBand].on && whiskerRelevant (selType))
    {
        const auto wk = whiskerEnds (selBand, selSide);
        const bool onRight = e.position.getDistanceFrom (wk.second) < kNodeR + 4.0f;
        const bool onLeft  = e.position.getDistanceFrom (wk.first)  < kNodeR + 4.0f;
        if (onLeft || onRight)
        {
            draggingBand = selBand; draggingSide = selSide; draggingQ = true; qDragSide = onRight ? 1 : -1;
            whiskerSlope = isCut (selType);
            if (auto* prm = proc.apvts.getParameter (laneParamId (selBand, selSide, whiskerSlope ? "slope" : "q")))
                prm->beginChangeGesture();
            lastDragFreq = (float) ((paramCache[(size_t) selBand].ms && selSide) ? paramCache[(size_t) selBand].sFreq
                                                                                 : paramCache[(size_t) selBand].freq);
            grabKeyboardFocus();
            if (e.mods.isAltDown()) driveAudition (true, lastDragFreq, audQSetting);
            return;
        }
    }

    // select the clicked node/lane (or clear on empty) so the toolbar follows, then start dragging
    selectBand (b, side);
    draggingBand = b; draggingSide = side;
    if (b >= 0)
    {
        const bool S  = paramCache[b].ms && side;
        const auto lt = S ? paramCache[b].sType : paramCache[b].type;
        pressBand = b; pressPos = e.position; pressMs = juce::Time::getMillisecondCounter(); pressMoved = false;
        if (auto* fp = proc.apvts.getParameter (laneParamId (b, side, "freq"))) fp->beginChangeGesture();
        draggingGain = hasGain (lt);
        if (draggingGain)
            if (auto* gp = proc.apvts.getParameter (laneParamId (b, side, "gain"))) gp->beginChangeGesture();
        lastDragFreq = (float) (S ? paramCache[b].sFreq : paramCache[b].freq);
        grabKeyboardFocus();
        if (e.mods.isAltDown()) driveAudition (true, lastDragFreq, audQSetting);
    }
}

void EqCurveDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (placing)   // press-drag placement: gain follows Y, freq follows X; preview only (commit on release)
    {
        placePos = e.position;
        if (e.position.getDistanceFrom (placeDown) > 4.0f) placeMoved = true;
        const bool lockY = e.mods.isAltDown() && audLockGain;
        placeGainFromDrag = placeMoved && ! lockY;                                            // Alt+lock -> gain stays default
        lastDragFreq = (float) xToFreq (placePos.x);
        driveAudition (e.mods.isAltDown(), lastDragFreq, audQSetting);                        // Alt = listen to the region
        repaint();
        return;
    }
    if (draggingBand < 0) return;
    if (pressBand >= 0 && e.position.getDistanceFrom (pressPos) > 4.0f) pressMoved = true;   // a real drag cancels long-press
    const bool lockY = e.mods.isAltDown() && audLockGain;                                     // Alt+lock -> sweep freq only
    const bool S = paramCache[(size_t) draggingBand].ms && draggingSide;                      // which lane is being dragged
    const double laneF0 = S ? paramCache[(size_t) draggingBand].sFreq : paramCache[(size_t) draggingBand].freq;
    lastDragFreq = (float) (draggingQ ? laneF0 : xToFreq (e.position.x));
    driveAudition (e.mods.isAltDown(), lastDragFreq, audQSetting);                            // Alt = narrow band-listen
    if (draggingQ)      // Neutron-style Q-whisker drag: handle distance from centre -> bandwidth -> Q
    {
        const double fx = xToFreq (e.position.x);
        // Clamp the grabbed handle to its own side of the node (no jumping across), then map the
        // half-bandwidth to Q with the calibrated curve (hits 40 / 0.1 at the ends).
        const double bw = (qDragSide > 0) ? std::log2 (juce::jmax (laneF0, fx) / laneF0)
                                          : std::log2 (laneF0 / juce::jmin (laneF0, fx));
        if (whiskerSlope)   // HP/LP -> snap to the discrete slope list
            setParam (laneParamId (draggingBand, draggingSide, "slope"), (double) slopeIndexForBw (bw));
        else                // others -> continuous Q (calibrated, hits 40 / 0.1 at the ends)
            setParam (laneParamId (draggingBand, draggingSide, "q"), juce::jlimit (0.1, 40.0, whiskerQForBw (bw)));
        positionToolbar();   // follow the node while dragging it by mouse
        return;
    }
    setParam (laneParamId (draggingBand, draggingSide, "freq"), xToFreq (e.position.x));
    if (draggingGain && ! lockY)   // latched at mouseDown; Alt+lock sweeps frequency only (gain frozen)
        setParam (laneParamId (draggingBand, draggingSide, "gain"), juce::jlimit (-kGainRange, kGainRange, yToDb (e.position.y)));
    positionToolbar();   // follow the node while dragging it by mouse (cursor is on the node, not the bar)
}

void EqCurveDisplay::mouseUp (const juce::MouseEvent& e)
{
    driveAudition (false);   // stop any drag-audition
    if (placing)   // release inside -> commit the previewed band; outside -> cancel
    {
        if (getLocalBounds().toFloat().contains (e.position))
        {
            if (placeGainFromDrag && placeSpec.hasGainCtrl)
                addBandOfType (placeSpec.typeIndex, placePos, placeSpec.slopeIndex);                              // gain from drag Y
            else
                addBandOfType (placeSpec.typeIndex, { placePos.x, dbToY (placeSpec.gainDb) }, placeSpec.slopeIndex);  // freq from X, default gain
        }
        placing = false; placeMoved = false; repaint();
        return;   // the new band's selectBand() already placed the toolbar
    }
    endDragGesture();
    positionToolbar();   // canvas node-drag: re-snap to the node AFTER the drag ends (never during)
}

bool EqCurveDisplay::keyPressed (const juce::KeyPress& k)
{
    const int  code = k.getKeyCode();                 // compare the key alone; check modifiers separately
    const bool alt  = k.getModifiers().isAltDown();   // (k == KeyPress(code) also compares modifiers — a trap)

    if (code == 'F' && onToggleFullscreen) { onToggleFullscreen(); return true; }   // f toggles fullscreen

    if (code == juce::KeyPress::escapeKey)
    {
        if (juce::Desktop::getInstance().getKioskModeComponent() != nullptr)
        { juce::Desktop::getInstance().setKioskModeComponent (nullptr); return true; }   // Esc exits fullscreen
        if (placing) { placing = false; placeMoved = false; driveAudition (false); repaint(); return true; }
    }

    // Alt + Left/Right steps the band selection (and picks the first band if none is selected yet).
    if (alt && code == juce::KeyPress::leftKey)  { stepSelection (-1); return true; }
    if (alt && code == juce::KeyPress::rightKey) { stepSelection (+1); return true; }

    if (selBand >= 0 && selBand < tabby::kNumBands)   // hotkeys for the selected node
    {
        if (code == juce::KeyPress::backspaceKey || code == juce::KeyPress::deleteKey)
        {
            setParamGestured (tabby::bandId (selBand, "on"), 0.0);   // remove the band (free the slot)
            selectBand (-1);
            return true;
        }

        refreshDesigns();
        const auto& p  = paramCache[(size_t) selBand];
        const bool   S  = p.ms && selSide;                       // act on the focused lane
        const auto   lt = S ? p.sType   : p.type;
        const double lf = S ? p.sFreq   : p.freq;
        const double lg = S ? p.sGainDb : p.gainDb;
        const double lq = S ? p.sQ      : p.Q;
        const int    ls = S ? p.sSlope  : p.slope;

        if (! alt)
        {
            const double fStep = std::exp2 (1.0 / 24.0);                                // freq nudge: a quarter-tone
            if (code == juce::KeyPress::leftKey)  { setParamGestured (laneParamId (selBand, selSide, "freq"), juce::jlimit (kFreqMin, kFreqMax, lf / fStep)); positionToolbar(); return true; }
            if (code == juce::KeyPress::rightKey) { setParamGestured (laneParamId (selBand, selSide, "freq"), juce::jlimit (kFreqMin, kFreqMax, lf * fStep)); positionToolbar(); return true; }
            if ((code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) && hasGain (lt))
            {
                setParamGestured (laneParamId (selBand, selSide, "gain"),
                                  juce::jlimit (-kGainRange, kGainRange, lg + (code == juce::KeyPress::upKey ? 0.5 : -0.5)));
                positionToolbar();
                return true;                                                            // gain +/- 0.5 dB
            }
        }
        else   // Alt + Up/Down -> Q (or slope for HP/LP);  Alt+Left/Right handled above
        {
            if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
            {
                const int d = (code == juce::KeyPress::upKey) ? +1 : -1;
                if (isCut (lt))                                                         // HP/LP -> step the slope (octaves)
                    setParamGestured (laneParamId (selBand, selSide, "slope"), (double) juce::jlimit (0, 6, slopeIndexFromDb ((int) ls) + d));
                else                                                                    // others -> Q +/- 0.1
                    setParamGestured (laneParamId (selBand, selSide, "q"), juce::jlimit (0.05, 40.0, lq + 0.1 * d));
                positionToolbar();
                return true;
            }
        }
    }
    return false;
}

void EqCurveDisplay::modifierKeysChanged (const juce::ModifierKeys& mods)
{
    if (placing || draggingBand >= 0)                          // toggle the listen the instant Alt changes
        driveAudition (mods.isAltDown(), lastDragFreq, audQSetting);
    if (placing)
        placeGainFromDrag = placeMoved && ! (mods.isAltDown() && audLockGain);
}

void EqCurveDisplay::mouseMove (const juce::MouseEvent& e) { hoverPos = e.position; repaint(); }
void EqCurveDisplay::mouseExit (const juce::MouseEvent&)   { hoverPos = { -1.0f, -1.0f }; repaint(); }

juce::Point<float> EqCurveDisplay::addButtonAt() const noexcept
{
    if (addLine == AddLine::Off)                          return { -1.0f, -1.0f };
    if (hoverPos.x < 0.0f || draggingBand >= 0 || placing) return { -1.0f, -1.0f };
    if (nodeAt (hoverPos).band >= 0)                     return { -1.0f, -1.0f };   // on a node -> no "+"

    // don't surface "+" over the selected lane's whisker bar (so you can grab a handle)
    const auto selType = (selBand >= 0 && paramCache[(size_t) selBand].ms && selSide) ? paramCache[(size_t) selBand].sType
                       : (selBand >= 0 ? paramCache[(size_t) selBand].type : teq::FilterType::Bell);
    if (selBand >= 0 && paramCache[(size_t) selBand].on && whiskerRelevant (selType))
    {
        const auto wk = whiskerEnds (selBand, selSide);
        const float ny = nodePos (selBand, selSide).y;
        if (std::abs (hoverPos.y - ny) < 12.0f && hoverPos.x > wk.first.x - 8.0f && hoverPos.x < wk.second.x + 8.0f)
            return { -1.0f, -1.0f };
    }

    bool anyFree = false;
    for (int i = 0; i < tabby::kNumBands; ++i) if (! paramCache[(size_t) i].on) { anyFree = true; break; }
    if (! anyFree)                                        return { -1.0f, -1.0f };   // all bands used

    // surface "+" when the cursor is within reach of an enabled trigger line (0 dB and/or the curve)
    const float yZero  = dbToY (0.0);
    const float yCurve = dbToY (compositeDb (xToFreq (hoverPos.x)));
    float best = kAddThreshold;
    bool  hit  = false;
    if (addLine == AddLine::ZeroLine || addLine == AddLine::Both) { const float d = std::abs (hoverPos.y - yZero);  if (d < best) { best = d; hit = true; } }
    if (addLine == AddLine::Curve    || addLine == AddLine::Both) { const float d = std::abs (hoverPos.y - yCurve); if (d < best) { best = d; hit = true; } }
    if (! hit)                                            return { -1.0f, -1.0f };
    return hoverPos;   // "+" sits at the cursor; predictAdd reads its Y for above/below 0
}

void EqCurveDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    refreshDesigns();
    const Hit hit = nodeAt (e.position);
    if (hit.band >= 0)                                            // on a node -> toggle that lane's bypass (ghost)
    {
        const bool byp = (paramCache[(size_t) hit.band].ms && hit.side) ? paramCache[(size_t) hit.band].sBypass
                                                                        : paramCache[(size_t) hit.band].bypass;
        setParamGestured (laneParamId (hit.band, hit.side, "bypass"), byp ? 0.0 : 1.0);
        selectBand (hit.band, hit.side);
    }
    else
        smartAdd (e.position);                                   // empty -> add (edge -> HP/LP, else Bell)
}

void EqCurveDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    refreshDesigns();
    const Hit hit = draggingBand >= 0 ? Hit { draggingBand, draggingSide } : nodeAt (e.position);
    if (hit.band < 0) return;
    auto* qp = proc.apvts.getParameter (laneParamId (hit.band, hit.side, "q"));
    if (qp == nullptr) return;
    const double cur    = qp->convertFrom0to1 (qp->getValue());   // live value (not the timer cache)
    const double factor = wheel.deltaY > 0.0f ? 1.15 : 1.0 / 1.15;
    setParamGestured (laneParamId (hit.band, hit.side, "q"), juce::jlimit (0.05, 40.0, cur * factor));
}
