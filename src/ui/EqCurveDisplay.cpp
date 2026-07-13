// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "ui/EqCurveDisplay.h"
#include "ui/Palette.h"
#include "ui/FilterShapes.h"
#include "ui/LaneMenu.h"

namespace
{
    // Node/whisker geometry + filter-type editing classification live in eqview::HandleMath
    // (JUCE-free, unit-tested); pull the names into this TU so every call-site stays unchanged.
    using eqview::handles::hasGain;
    using eqview::handles::qRelevant;
    using eqview::handles::isCut;
    using eqview::handles::slopeWhisker;
    using eqview::handles::whiskerRelevant;
    using eqview::handles::whiskerBwForQ;
    using eqview::handles::whiskerQForBw;
    using eqview::handles::slopeIndexFromDb;
    using eqview::handles::slopeBwForIndex;
    using eqview::handles::slopeIndexForBw;
    using eqview::handles::kSlopeDb;
    using eqview::handles::restsOnZeroDb;

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
    analyzer.peakFallDb = kPeakFallDb;   // the pane readies its own Hann window / FFT plan / dB arrays
    proc.setAnalyzerActive (true);
    setWantsKeyboardFocus (true);     // so Esc can cancel an in-progress add-drag

    // Vertical-scale picker (top-right overlay): ±3 / ±6 / ±12 / ±30 dB.
    for (int i = 0; i < 4; ++i) gainScaleCombo.addItem (juce::String ((int) kGainSteps[i]) + " dB", i + 1);
    gainScaleCombo.setColour (juce::ComboBox::textColourId,       tabby::palette::text());
    gainScaleCombo.setColour (juce::ComboBox::backgroundColourId, tabby::palette::panel().withAlpha (0.85f));
    gainScaleCombo.setColour (juce::ComboBox::outlineColourId,    tabby::palette::panel().brighter (0.20f));
    gainScaleCombo.setColour (juce::ComboBox::arrowColourId,      tabby::palette::textDim());
    gainScaleCombo.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (gainScaleCombo);
    gainScaleCombo.onChange = [this] { const int i = gainScaleCombo.getSelectedItemIndex(); if (i >= 0) setGainRange (kGainSteps[i]); };
    setGainRange ((double) proc.apvts.state.getProperty ("gainRangeLive",              // restore the VISIBLE scale (auto-fit
                      proc.apvts.state.getProperty ("gainRange", 12.0)), false);       // included), like the post-apply re-sync
    { refreshDesigns(); double mx = 0.0;                                               // then fit: bump out until the biggest saved gain shows
      for (int b = 0; b < tabby::kNumBands; ++b) if (traces.param (b).on)
          for (int L = 0; L < teq::kNumLanes; ++L) if (traces.param (b).lanes[L].on) mx = juce::jmax (mx, std::abs (traces.param (b).lanes[L].gainDb));
      while (gainRange < kGainSteps[3] && mx > gainRange) setGainRange (nextGainStep (gainRange), false); }

    startTimerHz (30);
}

EqCurveDisplay::~EqCurveDisplay()
{
    stopTimer();
    endDragGesture();                 // never leave a host automation gesture open
    proc.setAnalyzerActive (false);
}

//==============================================================================
// The freq↔px / dB↔px math lives in eqview::PlotMap (src/eqview/PlotMap.h, unit-tested) — these
// wrappers only feed it the component's live geometry, so every caller keeps its old signature.
eqview::PlotMap EqCurveDisplay::plotMap() const noexcept
{
    const eqview::PlotMap pm { .width = (float) getWidth(), .height = (float) getHeight(),
                               .plotBottom = (float) plotBottomY(),
                               .freqMin = kFreqMin, .freqMax = kFreqMax, .dbRange = gainRange,
                               .specTop = kSpecTop, .specBottom = kSpecBottom };
    // The unit test can't see THIS wiring (it hand-builds maps) — so sanity-pin it here, debug-only:
    // a scrambled field mapping trips on the first paint instead of silently moving pixels.
    // plotBottom may legally EXCEED a sub-40px height (plotBottomY floors at 40 — collapse/mid-layout
    // transients), so that check stands down for tiny heights instead of aborting a legal state.
    jassert ((pm.plotBottom <= pm.height || pm.height < 40.0f)
             && pm.freqMin < pm.freqMax && pm.specBottom < pm.specTop && pm.dbRange > 0.0);
    return pm;
}

float  EqCurveDisplay::freqToX (double f) const noexcept { return plotMap().freqToX (f); }
double EqCurveDisplay::xToFreq (float x)  const noexcept { return plotMap().xToFreq (x); }
float  EqCurveDisplay::dbToY   (double db) const noexcept { return plotMap().dbToY (db); }
double EqCurveDisplay::yToDb   (float y)   const noexcept { return plotMap().yToDb (y); }
float  EqCurveDisplay::specDbToY (double db) const noexcept { return plotMap().specDbToY (db); }

// Bottom of the curve/node plotting area. In Fixed-lane mode it stops above the reserved bottom strip, so
// nodes (whose Y comes from dbToY) can never fall into the lane; every other mode uses the full height.
int    EqCurveDisplay::plotBottomY() const noexcept
{
    // The NOMINAL +/-gainRange scale (dbToY, node positions, the drag-to-rescale boundary) stops a strip short
    // of the window bottom. Below that invisible -gainRange line the curves OVERSHOOT (a deep cut / notch dives
    // on down to the very bottom of the window), the spectrum shows, and in Fixed-lane the edit strip slides.
    // The curve *clip* uses the full window height (see paint), so curves reach the bottom edge, not this line.
    // Stays PRODUCT-side (not in PlotMap): it depends on the toolbar-placement mode.
    return toolbarPlace == ToolbarPlace::FixedLane ? juce::jmax (40, getHeight() - kLaneH)
                                                   : juce::jmax (40, getHeight() - kBottomAxisH);
}

//==============================================================================
// Placement-lane node/lane helpers (ST/L/R/M/S). A node exists per ENABLED lane. A point looks unsplit
// (per-band colour, no badge) ONLY in the plain single-ST configuration; any other lane set — including a
// single non-ST lane ({m}-only after a migration fission, Alt-click make-only) — wears lane colours and
// the badge, because such a point behaves differently from a plain one (it touches only its domain) and
// hiding that would misread as a normal full-stereo band.
bool EqCurveDisplay::laneOn (int b, int lane) const noexcept
{
    return traces.param (b).lanes[(size_t) lane].on;
}
bool EqCurveDisplay::laneOnLive (int b, int lane) const noexcept
{
    auto* a = proc.apvts.getRawParameterValue (tabby::laneParamId (b, lane, "on"));
    return a != nullptr && a->load() > 0.5f;
}
int EqCurveDisplay::laneCount (int b) const noexcept
{
    int c = 0; for (int L = 0; L < teq::kNumLanes; ++L) if (traces.param (b).lanes[(size_t) L].on) ++c;
    return c;
}
bool EqCurveDisplay::multiLane (int b) const noexcept
{
    return laneCount (b) >= 2 || ! traces.param (b).lanes[(size_t) teq::Lane::Stereo].on;
}

// The stereo display axis a lane rides (decision #7). The Stereo lane folds into every axis, so its own
// node/curve rides the hero (Mid) axis — exactly what today's single-ST composite is.
teq::Axis EqCurveDisplay::axisForLane (int lane) noexcept
{
    switch ((teq::Lane) lane)
    {
        case teq::Lane::Left:  return teq::Axis::Left;
        case teq::Lane::Right: return teq::Axis::Right;
        case teq::Lane::Side:  return teq::Axis::Side;
        case teq::Lane::Mid:
        case teq::Lane::Stereo:
        default:               return teq::Axis::Mid;
    }
}
const char* EqCurveDisplay::laneKeyStr (int lane) noexcept
{
    switch (lane) { case 1: return "l"; case 2: return "r"; case 3: return "m"; case 4: return "s"; default: return "st"; }
}
juce::String EqCurveDisplay::laneParamId (int b, int lane, juce::StringRef base) const
{
    const juce::String bs (base);
    if (bs == "type") return tabby::bandId (b, "type");   // one SHARED type per point (decision #2)
    if (bs == "on")   return tabby::bandId (b, "on");     // point exists
    const juce::String field = (bs == "bypass") ? "byp" : bs;   // node double-click toggles the LANE ghost
    return tabby::laneParamId (b, lane, field);
}

//==============================================================================
void EqCurveDisplay::refreshDesigns()
{
    // The snapshot/design/evaluation math lives in eqview::TraceSet (unit-tested); the component
    // only supplies its parameter SOURCE (the processor) and consumes curves/params back.
    traces.refresh (proc.getSampleRate(), [this] (int b) { return proc.readBand (b); });
}

// One stereo axis composite (decision #7): ∏ over bands of H_ST · H_axis (an inactive lane contributes
// identity). Evaluated from the pre-designed cache (fast); mirrors teq::compositeResponse exactly.
double EqCurveDisplay::compositeDbAxis (double f, teq::Axis a) const noexcept { return traces.compositeDbAxis (f, a); }

juce::Point<float> EqCurveDisplay::nodePos (int b, int lane) const noexcept
{
    // bells/shelves/tilt sit at their OWN gain (a drag tracks the cursor). HP/LP, notch and all-pass are
    // gain-less (cut/surgical/phase) so they always rest on the 0 dB line and never drag vertically; only
    // band-pass rides the composite at its corner (on that lane's display axis).
    const auto&  lp = traces.param (b).lanes[(size_t) lane];
    const auto   t  = traces.param (b).type;              // shared point type
    const double f  = lp.freq;
    const double g  = lp.gainDb;
    double db;
    if      (hasGain (t))       db = g;
    else if (restsOnZeroDb (t)) db = 0.0;
    else                        db = compositeDbAxis (f, axisForLane (lane));
    return { freqToX (f), dbToY (db) };
}

std::pair<juce::Point<float>, juce::Point<float>> EqCurveDisplay::whiskerEnds (int b, int lane) const noexcept
{
    const auto&  lp  = traces.param (b).lanes[(size_t) lane];
    const auto   pos = nodePos (b, lane);
    const double f0  = lp.freq;
    const auto   t   = traces.param (b).type;             // shared point type
    const int    sl  = lp.slope;
    const double qv  = lp.Q;
    const double bw  = eqview::handles::whiskerBw (t, qv, sl);                 // half-bandwidth (oct)
    const auto   e   = eqview::handles::whiskerEndsPx (plotMap(), { pos.x, pos.y }, f0, bw);
    return { { e.left.x, e.left.y }, { e.right.x, e.right.y } };
}

juce::Colour EqCurveDisplay::bandColour (int b) const noexcept
{
    if (perBandColors)   // hand-picked fixed colour per band slot
        return juce::Colour (kBandColours[juce::jlimit (0, tabby::kNumBands - 1, b)]);
    return typeColour (traces.param (b).type);
}

// A node's colour: its LANE colour once the point is split (≥ 2 lanes), else the per-band colour (unsplit
// points look exactly as today).
juce::Colour EqCurveDisplay::nodeColour (int b, int lane) const noexcept
{
    return multiLane (b) ? tabby::palette::lane (lane) : bandColour (b);
}

double EqCurveDisplay::bandDb (int b, double f, int lane) const noexcept { return traces.bandDb (b, f, lane); }

EqCurveDisplay::Hit EqCurveDisplay::nodeAt (juce::Point<float> p) const noexcept
{
    Hit best; float bestD = eqview::handles::grabRadiusSq (kNodeR, eqview::handles::kNodeGrabPad);
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (traces.param (b).on)
            for (int L = 0; L < teq::kNumLanes; ++L)
                if (laneOn (b, L))
                {
                    const float d = p.getDistanceSquaredFrom (nodePos (b, L));
                    if (d < bestD) { bestD = d; best = { b, L }; }
                }
    return best;
}

// Every node within the grab radius (coincident nodes — Link FQ stacks them exactly). A repeated click in
// place cycles through the returned set (see mouseDown).
int EqCurveDisplay::collectNodesAt (juce::Point<float> p, Hit* out, int maxOut) const noexcept
{
    int n = 0; const float r2 = eqview::handles::grabRadiusSq (kNodeR, eqview::handles::kNodeGrabPad);
    for (int b = 0; b < tabby::kNumBands && n < maxOut; ++b)
        if (traces.param (b).on)
            for (int L = 0; L < teq::kNumLanes && n < maxOut; ++L)
                if (laneOn (b, L) && p.getDistanceSquaredFrom (nodePos (b, L)) <= r2)
                    out[n++] = { b, L };
    return n;
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
            if (auto* prm = proc.apvts.getParameter (laneParamId (draggingBand, draggingLane, whiskerSlope ? "slope" : "q")))
                prm->endChangeGesture();
        }
        else
        {
            if (auto* fp = proc.apvts.getParameter (laneParamId (draggingBand, draggingLane, "freq"))) fp->endChangeGesture();
            if (draggingGain)
                if (auto* gp = proc.apvts.getParameter (laneParamId (draggingBand, draggingLane, "gain"))) gp->endChangeGesture();
        }
        proc.endHistoryGesture();   // commit the drag as its ONE labelled undo step (balanced with mouseDown)
    }
    if (momentarySolo) { proc.setSoloBand (prevSoloBand); momentarySolo = false; }   // release momentary solo
    pressBand    = -1;
    pressMoved   = false;
    draggingBand = -1;
    draggingLane = 0;
    draggingGain = false;
    draggingQ    = false;
    qDragSide    = 0;
    whiskerSlope = false;
}

void EqCurveDisplay::selectBand (int newSel, int lane)
{
    lane = juce::jlimit (0, teq::kNumLanes - 1, lane);
    // Never select a disabled lane: fall back to the lowest enabled lane on the point.
    if (newSel >= 0 && ! laneOn (newSel, lane))
        for (int L = 0; L < teq::kNumLanes; ++L) if (laneOn (newSel, L)) { lane = L; break; }
    if (newSel == selBand && lane == selLane) return;
    selBand = newSel;
    selLane = (newSel >= 0) ? lane : 0;
    if (onBandSelected) onBandSelected (selBand, selLane);   // updates the toolbar's controls + lane first
    positionToolbar();                                       // then place it near the new node
}

void EqCurveDisplay::setSelectedLane (int lane)             // the toolbar switched the active lane
{
    lane = juce::jlimit (0, teq::kNumLanes - 1, lane);
    if (lane == selLane) return;
    selLane = lane;
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
    const int W = kToolbarW;
    int tx = (int) (node.x - (float) W * 0.5f);
    int ty = (int) (node.y - kNodeR - 12.0f - kToolbarH);            // above the node...
    if (ty < 2) ty = (int) (node.y + kNodeR + 12.0f);               // ...or below if no room
    tx = juce::jlimit (2, juce::jmax (2, getWidth()  - W - 2), tx);
    ty = juce::jlimit (2, stripMaxY(), ty);
    return { tx, ty, W, kToolbarH };
}

// 1 — Anchor-to-open-side: don't straddle the node. Sit it just inside one horizontal end and extend into the
// larger canvas gap, so it covers (at most) the neighbours on the emptier side instead of both sides at once.
juce::Rectangle<int> EqCurveDisplay::placeAnchorSide (juce::Point<float> node) const noexcept
{
    const int   W     = kToolbarW;
    const bool right = node.x < (float) getWidth() * 0.5f;           // more room to the right → extend right
    int tx = right ? (int) (node.x - (float) W * 0.12f)
                   : (int) (node.x - (float) W * 0.88f);
    int ty = (int) (node.y - kNodeR - 12.0f - kToolbarH);
    if (ty < 2) ty = (int) (node.y + kNodeR + 12.0f);
    tx = juce::jlimit (2, juce::jmax (2, getWidth()  - W - 2), tx);
    ty = juce::jlimit (2, stripMaxY(), ty);
    return { tx, ty, W, kToolbarH };
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

    juce::Point<float> others[tabby::kNumBands * teq::kNumLanes]; int no = 0;   // every OTHER visible node centre
    for (int b = 0; b < tabby::kNumBands; ++b) if (traces.param (b).on)
        for (int L = 0; L < teq::kNumLanes; ++L)
            if (laneOn (b, L) && ! (b == selBand && L == selLane))
                others[no++] = nodePos (b, L);

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
    const int tx = juce::jlimit (2, juce::jmax (2, getWidth()  - (int) W - 2), (int) cand[best].x);
    const int ty = juce::jlimit (2, stripMaxY(), (int) cand[best].y);
    const juce::Rectangle<float> rc ((float) tx, (float) ty, W, H);
    occlOut = 0; for (int i = 0; i < no; ++i) if (rc.contains (others[i])) ++occlOut;
    return { tx, ty, (int) W, kToolbarH };
}

void EqCurveDisplay::positionToolbar()
{
    if (toolbar == nullptr) return;
    refreshDesigns();   // ensure the cache reflects a just-added/just-toggled band before we test it
    const bool show = selBand >= 0 && selBand < tabby::kNumBands && traces.param (selBand).on;
    if (! show) { toolbar->setVisible (false); lastLeaderBand = -2; lastToolbarPos = { -10000, -10000 }; return; }

    const auto node = nodePos (selBand, selLane);   // lane-aware: place at the actual selected node
    juce::Rectangle<int> b;

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
            break;
        }

        case ToolbarPlace::Hybrid:
        {
            int occl = 0, slot = 0;
            b = bestFloatCandidate (node, occl, slot);
            if (occl > 0)   // no clean local slot → dock to the far horizontal edge
            {
                const bool dockTop = node.y > (float) getHeight() * 0.5f;
                const int W  = kToolbarW;
                const int tx = juce::jlimit (2, juce::jmax (2, getWidth() - W - 2), (int) (node.x - (float) W * 0.5f));
                const int ty = dockTop ? 2 : stripMaxY();
                b = { tx, ty, W, kToolbarH };
                lastPlaceSlot = -2;   // docked
            }
            else lastPlaceSlot = slot;
            break;
        }

        case ToolbarPlace::FixedLane:
            // FabFilter-style: the strip lives in the reserved bottom lane and only slides horizontally to
            // track the selected band's frequency. Nodes can't enter the lane (dbToY squeezes them above it),
            // so the strip can never occlude a node; it sits at the lane top, clear of the freq axis below it.
            b = { juce::jlimit (2, juce::jmax (2, getWidth() - kToolbarW - 2), (int) (node.x - (float) kToolbarW * 0.5f)),
                  juce::jlimit (2, stripMaxY(), plotBottomY() + 3), kToolbarW, kToolbarH };
            break;

        case ToolbarPlace::Classic:
        default:
            b = placeClassic (node);
            break;
    }

    // Leader endpoints are kept for EVERY placement (the View option decides drawing). Flash re-arms on a
    // selection change or when the strip re-anchors/JUMPS (> 48 px in one step — a slot hop or a dock, not
    // the small continuous moves of a node drag).
    leaderNode = node; leaderBar = b.toFloat().getCentre();
    const bool selChanged = (selBand != lastLeaderBand || selLane != lastLeaderLane);
    const bool jumped     = lastToolbarPos.x > -9999
                         && b.getPosition().getDistanceFrom (lastToolbarPos) > 48;
    if (selChanged || jumped) leaderFlashMs = juce::Time::getMillisecondCounter();
    lastLeaderBand = selBand; lastLeaderLane = selLane; lastToolbarPos = b.getPosition();

    if (toolbar->getBounds() != b) toolbar->setBounds (b);
    if (! toolbar->isVisible()) toolbar->setVisible (true);
    toolbar->toFront (false);
}

void EqCurveDisplay::resized()
{
    gainScaleCombo.setBounds (getWidth() - 82, 6, 74, 22);   // top-right overlay, above the dB scale
    positionToolbar();
}

// Lowest allowed toolbar TOP: keeps the strip's bottom above the freq-axis label strip at the very bottom.
int EqCurveDisplay::stripMaxY() const noexcept { return juce::jmax (2, getHeight() - kToolbarH - kBottomAxisH); }

double EqCurveDisplay::nextGainStep (double r) noexcept   // smallest step strictly greater than r (clamps at the max)
{
    for (double s : kGainSteps) if (s > r + 0.01) return s;
    return kGainSteps[3];
}
int EqCurveDisplay::gainStepIndex() const noexcept
{
    int best = 0; double bd = 1.0e9;
    for (int i = 0; i < 4; ++i) { const double d = std::abs (kGainSteps[i] - gainRange); if (d < bd) { bd = d; best = i; } }
    return best;
}
void EqCurveDisplay::setGainRange (double r, bool persist)
{
    double snapped = kGainSteps[0], bd = 1.0e9;                       // snap to the nearest defined step
    for (double s : kGainSteps) { const double d = std::abs (s - r); if (d < bd) { bd = d; snapped = s; } }
    gainRange = snapped;
    // Suppressed: the vertical scale is VIEW state, not an edit (same policy as the lane tabs) —
    // unsuppressed, every zoom would settle into a junk "Parameter Change" step and Cmd+Z after
    // zooming would revert... the zoom. The properties still ride the snapshot for save/registers.
    // Value-equality skips normalize a String-typed post-load property without a "changed" write
    // (a retype counts as a forward move and would wipe an armed redo — same guard as activeLane).
    const felitronics::appkit::CompareHistory::ScopedSuppress ss (proc.compareHistory());
    if (std::abs ((double) proc.apvts.state.getProperty ("gainRangeLive", -1.0e9) - gainRange) > 1.0e-6)
        proc.apvts.state.setProperty ("gainRangeLive", gainRange, nullptr);   // the VISIBLE range, persist or not —
                                                                              // the lane menu's split-delta reads what
                                                                              // the user actually SEES (auto-fit included)
    gainScaleCombo.setSelectedItemIndex (gainStepIndex(), juce::dontSendNotification);
    if (persist && std::abs ((double) proc.apvts.state.getProperty ("gainRange", -1.0e9) - gainRange) > 1.0e-6)
        proc.apvts.state.setProperty ("gainRange", gainRange, nullptr);
    positionToolbar();   // dbToY changed → node/strip positions moved
    repaint();
}

void EqCurveDisplay::clearSelection() { selectBand (-1); }

void EqCurveDisplay::refreshToolbar() { positionToolbar(); }   // re-place after a window-slider edit ends

// A lane set / link edit happened elsewhere (the lane menu / wheel): re-cache, re-sync the strip to the
// (possibly re-clamped) selection, re-place the toolbar and repaint.
void EqCurveDisplay::refreshAfterLaneEdit()
{
    refreshDesigns();
    if (selBand >= 0)
    {
        if (! traces.param (selBand).on) { selectBand (-1); repaint(); return; }
        // Adopt the STATE's active lane (state → UI). After an undo/switch/load the restored
        // property is authoritative — re-firing the display's CACHED lane would push it back into
        // the fresh snapshot as a suppressed forward move, clearing the just-built redo (crew P1).
        const int stateLane = (int) proc.apvts.state.getProperty (tabby::bandId (selBand, "activeLane"), selLane);
        if (stateLane >= 0 && stateLane < teq::kNumLanes && laneOn (selBand, stateLane))
            selLane = stateLane;
        if (! laneOn (selBand, selLane))
            for (int L = 0; L < teq::kNumLanes; ++L) if (laneOn (selBand, L)) { selLane = L; break; }
        if (onBandSelected) onBandSelected (selBand, selLane);   // re-bind the strip to the current band/lane
    }
    positionToolbar();
    repaint();
}

// Step across ALL visible nodes in GLOBAL frequency order (tiebreak: band index, then lane order), wrapping.
void EqCurveDisplay::stepSelection (int dir)
{
    refreshDesigns();
    struct Node { int b, l; double f; };
    Node nodes[tabby::kNumBands * teq::kNumLanes]; int n = 0;
    for (int b = 0; b < tabby::kNumBands; ++b) if (traces.param (b).on)
        for (int L = 0; L < teq::kNumLanes; ++L) if (laneOn (b, L))
            nodes[n++] = { b, L, traces.param (b).lanes[(size_t) L].freq };
    if (n == 0) return;
    for (int i = 1; i < n; ++i)                       // insertion sort: freq, then band, then lane
    {
        const Node k = nodes[i]; int j = i - 1;
        auto less = [] (const Node& a, const Node& c)   // freq, then band, then lane (no float == : ordered compares only)
        {
            if (a.f < c.f) return true;
            if (c.f < a.f) return false;
            return a.b != c.b ? a.b < c.b : a.l < c.l;
        };
        while (j >= 0 && less (k, nodes[j])) { nodes[j + 1] = nodes[j]; --j; }
        nodes[j + 1] = k;
    }
    int cur = -1;
    for (int i = 0; i < n; ++i) if (nodes[i].b == selBand && nodes[i].l == selLane) { cur = i; break; }
    const int next = (cur < 0) ? (dir > 0 ? 0 : n - 1) : (((cur + dir) % n) + n) % n;
    selectBand (nodes[next].b, nodes[next].l);
}

//==============================================================================
void EqCurveDisplay::pushSpectrum()
{
    // The pane owns the pipeline (window -> FFT -> dB smoothing + peak-hold, unit-tested);
    // this component owns only the SOURCE — the processor's pre/post tap.
    if (proc.pullSpectrum (analyzerPre, analyzer.frameInput())) analyzer.ingest();
    else                                                        analyzer.starve();
}

// Spectrum on the display's log grid: linear-interpolate the sparse bass (smooth, no stair-steps),
// take the bin MAX in the dense highs (keeps the spiky harmonic detail pro analyzers show). High
// column resolution (~1/px) so high-freq peaks aren't skipped. +4.5 dB/oct pink-tilt on top.
// The column math lives in the pane; here we only wrap the emitted points into JUCE paths. The
// geometry has ONE source — the PlotMap — for both the columns and the closing edges (a separate
// width parameter could silently detach the fill's closing edge from the last column).
void EqCurveDisplay::buildSpectrumPaths (juce::Path& fillOut, juce::Path& peakOut) const
{
    const auto pm = plotMap();
    float lastY = pm.height;
    analyzer.buildColumns (pm, traces.sampleRate(), kTiltDbPerOct, kTiltPivotHz,
        [&] (int i, float x, float yS, float yP)
        {
            if (i == 0) { fillOut.startNewSubPath (0.0f, pm.height); fillOut.lineTo (0.0f, yS); peakOut.startNewSubPath (x, yP); }
            else          peakOut.lineTo (x, yP);
            fillOut.lineTo (x, yS);
            lastY = yS;
        });
    fillOut.lineTo (pm.width, lastY); fillOut.lineTo (pm.width, pm.height); fillOut.closeSubPath();
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
    const auto   pm = plotMap();   // hoisted: one geometry snapshot for the whole overlay
    const double f0 = juce::jlimit (kFreqMin, kFreqMax, f0c);
    const float  xc = pm.freqToX (f0);
    const auto   orng = tabby::palette::orange();

    if (audVisual == AudVisual::Bell)
    {
        teq::BandParams bp;
        bp.on = true; bp.type = teq::FilterType::Bell;
        auto& st = bp.lane (teq::Lane::Stereo);
        st.freq = f0; st.Q = juce::jmax (0.5, qc); st.gainDb = 20.0;         // a tall narrow bell = what you hear
        const float y0 = pm.dbToY (0.0);
        juce::Path line, fill; bool started = false;
        for (float x = 0.0f; x <= w; x += 3.0f)
        {
            const double dfs = designFs();
            const double wd = 2.0 * juce::MathConstants<double>::pi * juce::jmin (pm.xToFreq (x), 0.499 * dfs) / dfs;
            const double db = 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (teq::bandResponse (bp, dfs, wd))));
            const float  y  = pm.dbToY (db);
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
        const float  xLo = pm.freqToX (f0 * (k - inv));
        const float  xHi = pm.freqToX (f0 * (k + inv));
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
    const auto pm = plotMap();             // hoisted: one geometry snapshot for the whole paint pass
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
        const float x = pm.freqToX (f);
        g.setColour (tabby::palette::grid());
        g.drawVerticalLine ((int) x, 0.0f, h);
        g.setColour (tabby::palette::axisText());
        g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, (f == 1000.0 ? 0 : 0)) + "k" : juce::String ((int) f),
                    (int) x + 2, (int) h - 14, 34, 12, juce::Justification::left);
    }
    // dB grid + right-edge vertical scale — ticks adapt to the selected range (±3/6/12/30).
    const double tickStep = gainRange <= 3.0 ? 1.0 : gainRange <= 6.0 ? 2.0 : gainRange <= 12.0 ? 3.0 : 6.0;
    for (double db = -gainRange; db <= gainRange + 0.01; db += tickStep)
    {
        if (std::abs (std::abs (db) - gainRange) < 0.01) continue;   // skip the outermost ticks — they sit ON the top/bottom
                                                                     // edge and read as a frame the curves "bump into"
        const float y = pm.dbToY (db);
        const bool zero = std::abs (db) < 0.01;
        if (zero)
        {
            g.setColour (tabby::palette::violetLo().withAlpha (0.05f));   // soft glow on the 0 dB line
            g.fillRect (0.0f, y - 2.5f, w, 5.0f);
            g.setColour (tabby::palette::gridZero());
        }
        else
            g.setColour (tabby::palette::grid());
        g.drawHorizontalLine ((int) y, 0.0f, w);
        if (! zero && y >= 30.0f && y <= (float) plotBottomY() - 2.0f)   // dB value on the right (below the scale combo)
        {
            g.setColour (tabby::palette::axisText());
            g.drawText ((db > 0.0 ? "+" : "") + juce::String ((int) db), (int) w - 32, (int) y - 6, 28, 12, juce::Justification::right);
        }
    }

    // (The beyond-Nyquist fog is drawn LAST — over the curve so it dissolves into it, see the end of paint.)

    // --- spectrum (filled) ------------------------------------------------
    {
        juce::Path specFill, specPeakPath;
        buildSpectrumPaths (specFill, specPeakPath);
        g.setGradientFill (juce::ColourGradient (tabby::palette::spectrum().withAlpha (0.22f), 0.0f, h * 0.30f,
                                                 tabby::palette::spectrum().withAlpha (0.03f), 0.0f, h, false));
        g.fillPath (specFill);
        g.setColour (tabby::palette::spectrum().withAlpha (0.55f));
        g.strokePath (specPeakPath, juce::PathStrokeType (1.0f));
    }

    // Fixed-lane placement: mark the reserved bottom lane — nodes stay above this line, spectrum still shows below.
    if (toolbarPlace == ToolbarPlace::FixedLane)
    {
        g.setColour (tabby::palette::gridZero());
        g.drawHorizontalLine (plotBottomY(), 0.0f, w);
    }

    // Shared curve sample grid: a uniform log-x grid PLUS an exact sample at every active band's centre freq,
    // so a notch's -inf null (and bell peaks) are hit dead-on — the drawn tip no longer jitters between grid
    // points as you drag. cy() clamps dB to the plot floor so nulls bottom out cleanly and consistently.
    float curveXs[280 + tabby::kNumBands * teq::kNumLanes]; int nCurveX = 0;
    for (int i = 0; i <= 256; ++i) curveXs[nCurveX++] = (float) i / 256.0f * w;
    for (int b = 0; b < tabby::kNumBands; ++b) if (traces.param (b).on)
        for (int L = 0; L < teq::kNumLanes; ++L)
            if (laneOn (b, L)) curveXs[nCurveX++] = pm.freqToX (traces.param (b).lanes[(size_t) L].freq);
    std::sort (curveXs, curveXs + nCurveX);
    // Below the graph floor, let the curve DIVE off the bottom axis (it's clipped to the plot area, below)
    // instead of clamping it flat ONTO the bottom line — a deep cut / notch / LP slope should read as a
    // dive off the bottom, not a smear of every curve along the bottom edge.
    auto cy = [&] (double db) { return pm.dbToY (juce::jmax (-2.5 * gainRange, db)); };

    // --- per-band response curves (colour line + subtle fill); soloing -> only that band; the
    //     selected band's own curve is lifted ------------------------------------------------------
    if (perBandCurves || solo >= 0)
    {
        juce::Graphics::ScopedSaveState bandClip (g);
        g.reduceClipRegion (0, 0, (int) w, getHeight());   // curves dive to the WINDOW bottom, below the invisible -gainRange line
        const float y0 = pm.dbToY (0.0);
        auto drawLane = [&] (int b, int lane)
        {
            const auto& lp = traces.param (b).lanes[(size_t) lane];
            if (traces.param (b).bypass || lp.bypass || ! lp.on) return;    // point/lane bypass or disabled lane -> no curve
            juce::Path bc, bf;
            bf.startNewSubPath (0.0f, y0);
            for (int i = 0; i < nCurveX; ++i)
            {
                const float x = curveXs[i];
                const float y = cy (bandDb (b, pm.xToFreq (x), lane));
                if (i == 0) bc.startNewSubPath (x, y); else bc.lineTo (x, y);
                bf.lineTo (x, y);
            }
            bf.lineTo (w, y0); bf.closeSubPath();

            const bool hot = (solo == b) || (solo < 0 && b == selBand && lane == selLane);
            const auto col = nodeColour (b, lane);   // lane colour for split points (dimmed), else per-band
            if (perBandFill) { g.setColour (col.withAlpha (hot ? 0.12f : 0.05f)); g.fillPath (bf); }
            g.setColour (col.withAlpha (hot ? 0.70f : 0.32f)); g.strokePath (bc, juce::PathStrokeType (hot ? 1.4f : 0.9f));
        };
        for (int b = 0; b < tabby::kNumBands; ++b)
            if (traces.param (b).on && (solo < 0 || solo == b))
                for (int L = 0; L < teq::kNumLanes; ++L)
                    if (laneOn (b, L)) drawLane (b, L);
    }

    // --- response curve: warm/cool fill to 0 dB, faux-glow, crisp line ----
    {
        juce::Graphics::ScopedSaveState compositeClip (g);
        g.reduceClipRegion (0, 0, (int) w, getHeight());   // composite dives to the WINDOW bottom, below the invisible -gainRange line
        const float y0 = pm.dbToY (0.0);
        juce::Path line, fill;
        fill.startNewSubPath (0.0f, y0);
        for (int i = 0; i < nCurveX; ++i)
        {
            const float x = curveXs[i];
            const float y = cy (compositeDbAxis (pm.xToFreq (x), teq::Axis::Mid));   // hero = the Mid axis (== today's main)
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

        // Additional dimmed AXIS composites (decision #7): the L / R / S axes, drawn only where some band has
        // an active own-domain lane on that axis (the Mid axis IS the orange hero above). Each in its lane
        // colour with a small letter at the right edge. Everything collapses to the single hero when unsplit.
        const teq::Axis extra[3] = { teq::Axis::Left, teq::Axis::Right, teq::Axis::Side };
        for (const auto ax : extra)
        {
            const teq::Lane axl = teq::axisLane (ax);
            bool differs = false;
            for (int b = 0; b < tabby::kNumBands; ++b)
                if (traces.param (b).on && teq::laneActive (traces.param (b), axl)) { differs = true; break; }
            if (! differs) continue;

            juce::Path aline; bool st = false;
            for (int i = 0; i < nCurveX; ++i)
            {
                const float x = curveXs[i];
                const float y = cy (compositeDbAxis (pm.xToFreq (x), ax));
                if (! st) { aline.startNewSubPath (x, y); st = true; } else aline.lineTo (x, y);
            }
            const auto col = tabby::palette::lane ((int) axl);
            g.setColour (col.withAlpha (dim ? 0.35f : 0.80f));
            g.strokePath (aline, juce::PathStrokeType (1.4f));

            const float labY = cy (compositeDbAxis (pm.xToFreq (w - 10.0f), ax));   // subtle right-edge label
            g.setFont (juce::Font (juce::FontOptions (10.0f).withStyle ("Bold")));
            g.setColour (col.withAlpha (dim ? 0.55f : 0.95f));
            g.drawText (juce::String (laneKeyStr ((int) axl)).toUpperCase(),
                        (int) w - 22, juce::jlimit (2, (int) h - 14, (int) labY - 7), 18, 12, juce::Justification::right);
        }
    }

    // --- hover halo on the node under the cursor --------------------------
    if (hoverPos.x >= 0.0f && draggingBand < 0)
    {
        const Hit hb = nodeAt (hoverPos);
        if (hb.band >= 0)
        {
            const auto hp = nodePos (hb.band, hb.lane);
            g.setColour (nodeColour (hb.band, hb.lane).withAlpha (0.22f));
            g.fillEllipse (hp.x - kNodeR - 7, hp.y - kNodeR - 7, (kNodeR + 7) * 2, (kNodeR + 7) * 2);
        }
    }

    // --- toolbar leader line: a thin node -> strip connector, uniform across every placement mode.
    //     View option: Off / Flash (~1 s with a fade tail on selection change / strip jump) / Always. ---
    if (toolbar != nullptr && toolbar->isVisible() && leaderOpt != Leader::Off)
    {
        float env = 1.0f;
        if (leaderOpt == Leader::Flash)
        {
            const juce::uint32 dt = juce::Time::getMillisecondCounter() - leaderFlashMs;
            env = dt >= 1000 ? 0.0f : dt <= 600 ? 1.0f : 1.0f - (float) (dt - 600) / 400.0f;   // hold 0.6 s, fade 0.4 s
        }
        if (env > 0.01f)
        {
            g.setColour (tabby::palette::violetLo().withAlpha (0.50f * env));
            g.drawLine (leaderNode.x, leaderNode.y, leaderBar.x, leaderBar.y, 1.4f);
            g.setColour (tabby::palette::violetLo().withAlpha (0.90f * env));
            g.fillEllipse (leaderNode.x - 2.5f, leaderNode.y - 2.5f, 5.0f, 5.0f);
        }
    }

    // --- nodes (one per enabled lane; ≥ 2 lanes -> lane colour + permanent badge) ------------------
    auto drawNode = [&] (int b, int lane)
    {
        const auto pos = nodePos (b, lane);
        const auto col = nodeColour (b, lane);
        const auto numR = juce::Rectangle<float> (pos.x - kNodeR, pos.y - kNodeR, kNodeR * 2, kNodeR * 2);
        const bool byp = traces.param (b).bypass || traces.param (b).lanes[(size_t) lane].bypass;   // point or lane bypass -> ghost
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
        if (multiLane (b))   // permanent lane badge: a small letter chip glued to the node's top-right
        {
            const juce::String badge = juce::String (laneKeyStr (lane)).toUpperCase();
            const float bw = badge.length() > 1 ? 15.0f : 11.0f, bh = 10.0f;
            juce::Rectangle<float> chip (pos.x + kNodeR - 2.0f, pos.y - kNodeR - bh + 2.0f, bw, bh);
            if (chip.getY() < 1.0f) chip.setY (pos.y + kNodeR - 2.0f);   // top-edge clamp: flip below the node
            g.setColour (col);                               g.fillRoundedRectangle (chip, 2.5f);
            g.setColour (juce::Colours::black.withAlpha (0.85f));
            g.setFont (juce::Font (juce::FontOptions (8.0f).withStyle ("Bold")));
            g.drawText (badge, chip, juce::Justification::centred);
        }
    };
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (traces.param (b).on)
            for (int L = 0; L < teq::kNumLanes; ++L)
                if (laneOn (b, L)) drawNode (b, L);

    // --- selection highlight (the focused lane's node) --------------------
    const int rb = draggingBand >= 0 ? draggingBand : selBand;
    const int rl = draggingBand >= 0 ? draggingLane : selLane;
    if (rb >= 0 && rb < tabby::kNumBands && traces.param (rb).on && laneOn (rb, rl))
    {
        const auto pos = nodePos (rb, rl);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawEllipse (pos.x - kNodeR - 3, pos.y - kNodeR - 3, (kNodeR + 3) * 2, (kNodeR + 3) * 2, 1.5f);

        // Whiskers (Neutron-style): drag an end handle to set bandwidth/Q (HP/LP -> discrete slope)
        const auto laneType = traces.param (rb).type;   // shared point type
        if (whiskerRelevant (laneType))
        {
            const auto wk  = whiskerEnds (rb, rl);
            const auto col = nodeColour (rb, rl);
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
    else if (solo >= 0 && solo < tabby::kNumBands && traces.param (solo).on)
    {
        int sl = (int) proc.apvts.state.getProperty (tabby::bandId (solo, "activeLane"), 0);   // matches the audio solo lane
        if (sl < 0 || sl >= teq::kNumLanes || ! laneOn (solo, sl))
            for (int L = 0; L < teq::kNumLanes; ++L) if (laneOn (solo, L)) { sl = L; break; }
        drawListenVisual (g, traces.param (solo).lanes[(size_t) sl].freq, (double) audQSetting, "SOLO");
    }

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
            drawAddPreview (g, predictAdd (addBtn), addBtn, true);    // ghost passes THROUGH the cursor (gain from its Y)
            const float r = kNodeR + 2.0f;
            g.setColour (tabby::palette::panel().withAlpha (0.92f));    g.fillEllipse (addBtn.x - r, addBtn.y - r, r * 2, r * 2);
            g.setColour (tabby::palette::violetLo().withAlpha (0.95f)); g.drawEllipse (addBtn.x - r, addBtn.y - r, r * 2, r * 2, 1.5f);
            g.setColour (tabby::palette::text());
            g.drawLine (addBtn.x - 4.0f, addBtn.y, addBtn.x + 4.0f, addBtn.y, 1.6f);
            g.drawLine (addBtn.x, addBtn.y - 4.0f, addBtn.x, addBtn.y + 4.0f, 1.6f);
        }
    }

    // --- beyond-Nyquist fog (drawn LAST, OVER the curve so the phantom tail dissolves) ------------
    // Below fs/2 the (oversampled, see designFs) curve ~= the real response; above it there is no real
    // signal — the curve is pure analog intent, so let it fade into fog toward the right edge. A gradient
    // (clear at Nyquist -> near-opaque at 28k) thickens across the phantom zone; a hairline marks fs/2.
    // At 88.2/96k Nyquist sits past the 28k axis, so xNyq is off-screen and nothing draws; at 44.1/48k it
    // fogs the top sliver (the curve dissolves right at the edge); at low rates it fogs most of the top.
    if (traces.sampleRate() > 0.0)
    {
        const float xNyq = pm.freqToX (0.499 * traces.sampleRate());
        if (xNyq < w - 1.0f)
        {
            const auto fog = tabby::palette::bg().darker (0.35f);
            g.setGradientFill (juce::ColourGradient (fog.withAlpha (0.0f),  xNyq, 0.0f,
                                                     fog.withAlpha (0.94f), w,    0.0f, false));
            g.fillRect (xNyq, 0.0f, w - xNyq, h);
            g.setColour (tabby::palette::violetLo().withAlpha (0.5f));   // hairline at Nyquist — where real signal ends
            g.drawVerticalLine ((int) xNyq, 0.0f, h);
        }
    }
}

//==============================================================================
void EqCurveDisplay::addBandOfType (int typeIndex, juce::Point<float> at, int slopeIndex)
{
    refreshDesigns();
    for (int b = 0; b < tabby::kNumBands; ++b)
        if (! traces.param (b).on)
        {
            const auto ft = tabby::filterTypeFromChoice (typeIndex);
            proc.beginHistoryGesture ("Add Band " + juce::String (b + 1));  // the whole multi-param birth = ONE undo step
            setParamGestured (tabby::bandId (b, "type"), typeIndex);        // shared point type
            // A fresh band is an unsplit ST lane. Force the split lanes off (a vacated slot may carry stale
            // on flags), and edit the ST lane's fields.
            setParamGestured (tabby::laneParamId (b, 0, "on"), 1.0);
            for (int L = 1; L < teq::kNumLanes; ++L) setParamGestured (tabby::laneParamId (b, L, "on"), 0.0);
            setParamGestured (tabby::laneParamId (b, 0, "freq"), juce::jlimit (kFreqMin, kFreqPlaceMax, xToFreq (at.x)));
            if (ft == teq::FilterType::Bell || ft == teq::FilterType::LowShelf || ft == teq::FilterType::HighShelf)
                setParamGestured (tabby::laneParamId (b, 0, "gain"), juce::jlimit (-kGainMax, kGainMax, yToDb (at.y)));
            setParamGestured (tabby::laneParamId (b, 0, "q"), tabby::defaultQFor (ft));
            if (slopeIndex >= 0) setParamGestured (tabby::laneParamId (b, 0, "slope"), (double) slopeIndex);
            setParamGestured (tabby::laneParamId (b, 0, "byp"), 0.0);
            setParamGestured (tabby::bandId (b, "bypass"), 0.0);            // point bypass off
            // Fresh band = INHERITED link state: REMOVE any props a recycled slot carried, so the point
            // inherits the View defaults (absent = inherit; the first split materializes them; an
            // explicit pre-split toggle sticks). Writing false here would silently defeat the defaults.
            proc.apvts.state.removeProperty (tabby::bandId (b, "linkFq"), nullptr);
            proc.apvts.state.removeProperty (tabby::bandId (b, "linkQ"),  nullptr);
            setParamGestured (tabby::bandId (b, "on"), 1.0);               // point on
            proc.endHistoryGesture();
            selectBand (b);
            return;
        }
}

void EqCurveDisplay::drawAddPreview (juce::Graphics& g, const AddSpec& s, juce::Point<float> at, bool dragging) const
{
    const auto ft = tabby::filterTypeFromChoice (s.typeIndex);

    const double f0    = juce::jlimit (kFreqMin, kFreqPlaceMax, xToFreq (at.x));   // preview a placeable band (≤ 20k)
    const double gain  = s.hasGainCtrl ? (dragging ? juce::jlimit (-kGainMax, kGainMax, yToDb (at.y)) : s.gainDb) : 0.0;
    const int    slDb  = kSlopeDb[juce::jlimit (0, 6, s.slopeIndex < 0 ? 1 : s.slopeIndex)];

    teq::BandParams bp;
    bp.on   = true;
    bp.type = ft;
    { auto& st = bp.lane (teq::Lane::Stereo);
      st.freq = f0; st.Q = tabby::defaultQFor (ft); st.gainDb = gain; st.slope = slDb; }   // = the committed default, so the ghost matches the band it becomes

    const auto col = tabby::palette::violetLo();

    // ghost response curve — floor-clamped + sampled dead-on at f0 (so a preview notch bottoms out cleanly
    // at the plot floor exactly like the committed curves, not diving past it or jittering).
    juce::Path path;
    const float wpx = (float) getWidth();
    const float xf0 = freqToX (f0);
    bool started = false, placedF0 = false;
    const double dfs = designFs();   // oversampled (see compositeDb) so a preview LP/BP dives smoothly past Nyquist
    auto emit = [&] (float x)
    {
        const double wd = 2.0 * juce::MathConstants<double>::pi * juce::jmin (xToFreq (x), 0.499 * dfs) / dfs;
        const double db = 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (teq::bandResponse (bp, dfs, wd))));
        const float  y  = dbToY (juce::jmax (-2.5 * gainRange, db));   // dive off the bottom (match the composite/per-band cy)
        if (! started) { path.startNewSubPath (x, y); started = true; } else path.lineTo (x, y);
    };
    for (float x = 0.0f; x <= wpx; x += 3.0f)
    {
        if (! placedF0 && x > xf0) { emit (xf0); placedF0 = true; }
        emit (x);
    }
    {
        juce::Graphics::ScopedSaveState ghostClip (g);
        g.reduceClipRegion (0, 0, (int) wpx, getHeight());   // ghost dives to the window bottom too (match the curves)
        g.setColour (col.withAlpha (0.5f));
        g.strokePath (path, juce::PathStrokeType (1.4f));
    }

    // node dot (matches nodePos: hasGain -> own gain; HP/LP/notch/all-pass -> 0; only band-pass its corner)
    double nodeDb = 0.0;
    if (hasGain (ft)) nodeDb = gain;
    else if (! restsOnZeroDb (ft))
        nodeDb = 20.0 * std::log10 (juce::jmax (1.0e-9, std::abs (
                     teq::bandResponse (bp, dfs, 2.0 * juce::MathConstants<double>::pi * f0 / dfs))));
    const float nx = freqToX (f0), ny = dbToY (nodeDb);
    g.setColour (col.withAlpha (0.9f));
    g.drawEllipse (nx - kNodeR, ny - kNodeR, kNodeR * 2.0f, kNodeR * 2.0f, 1.5f);

    // type icon + label
    const char* names[] = { "Bell", "Low Shelf", "High Shelf", "HPF", "LPF", "Band Pass", "Notch", "All Pass", "Tilt" };
    juce::String label = names[juce::jlimit (0, 8, s.typeIndex)];
    if (isCut (ft)) label << " " << juce::String (slDb);
    label << "   " << (f0 >= 1000.0 ? juce::String (f0 / 1000.0, 2) + " kHz"
                                    : juce::String (juce::roundToInt (f0)) + " Hz");
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
    const double f  = xToFreq (at.x);
    const double db = yToDb (at.y);
    AddSpec s;

    // The bottom quarter of the display (a deep cut) is entirely filters, split at 4 kHz: left → HPF, right → LPF
    // (24 dB/oct, no gain). Drag a node low = "I want a filter", regardless of the finer freq bands.
    if (db < -gainRange * 0.9375)  // the very bottom 1/32 → a surgical Notch (band-reject, 12 dB/oct default), any frequency
    {
        s.typeIndex = 6; s.slopeIndex = 1; s.gainDb = 0.0; s.hasGainCtrl = false;
        return s;
    }
    if (db < -gainRange * 0.75)    // the bottom 1/8 → a filter split at 4 kHz (left → HPF, right → LPF)
    {
        s.typeIndex = (f < 4000.0) ? 3 : 4;   // HPF : LPF
        s.slopeIndex = 2;                     // 24 dB/oct
        s.gainDb = 0.0; s.hasGainCtrl = false;
        return s;
    }

    // Otherwise pick the type by frequency band (along the 0 line); shelf/bell boost-or-cut follows the side of
    // 0 the cursor is on. Purely positional (no de-dup) — you can stack the same type if you want.
    const bool   above0 = db >= 0.0;
    const double amt    = juce::jmax (0.5, gainRange / 6.0);   // default "amount" scales with the view (looks the same at any zoom)
    s.gainDb = above0 ? amt : -amt;

    if      (f <    30.0) { s.typeIndex = 3; s.slopeIndex = 2; s.gainDb = 0.0; s.hasGainCtrl = false; }   // < 30 Hz  → HPF 24
    else if (f <   100.0) { s.typeIndex = 1; }                                                            // 30–100   → Low Shelf
    else if (f <  8000.0) { s.typeIndex = 0; }                                                            // 100–8k   → Bell (dome)
    else if (f < 19000.0) { s.typeIndex = 2; }                                                            // 8k–19k   → High Shelf
    else                  { s.typeIndex = 4; s.slopeIndex = 2; s.gainDb = 0.0; s.hasGainCtrl = false; }   // > 19 kHz → LPF 24

    return s;
}

void EqCurveDisplay::smartAdd (juce::Point<float> at)
{
    refreshDesigns();
    const AddSpec s = predictAdd (at);
    addBandOfType (s.typeIndex, at, s.slopeIndex);   // freq + gain both come from the cursor (filters ignore gain)
}

void EqCurveDisplay::mouseDown (const juce::MouseEvent& e)
{
    // A second input source (touch/pen) landing mid-drag must not nest a second grab: it would
    // overwrite the drag state and leak the outer history gesture + the host change-gestures.
    if (draggingBand >= 0 || placing)
        return;
    dragSourceIndex = e.source.getIndex();   // whoever grabs owns the drag (see mouseDrag/mouseUp)

    refreshDesigns();
    const Hit  hit  = nodeAt (e.position);
    const int  b    = hit.band;
    const int  lane = hit.lane;

    if (e.mods.isPopupMenu())
    {
        const char* names[] = { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Band Pass", "Notch", "All Pass", "Tilt" };
        juce::Component::SafePointer<EqCurveDisplay> safe (this);
        juce::PopupMenu m;

        if (b >= 0)   // on a node: change type / placement lanes / remove
        {
            for (int i = 0; i < 9; ++i)
            {
                juce::PopupMenu::Item it;
                it.itemID = i + 1;
                it.text   = names[i];
                it.setImage (tabby::shapes::icon (tabby::filterTypeFromChoice (i), tabby::palette::violetLo()));
                it.setTicked (traces.param (b).type == tabby::filterTypeFromChoice (i));   // shared point type
                m.addItem (it);
            }
            m.addSeparator();
            juce::PopupMenu lanes;   // the SAME lane submenu the strip dropdown shows
            tabby::lanemenu::build (lanes, proc, b, proc.getTotalNumOutputChannels() == 2,
                [safe, b] (int ln) { if (safe != nullptr) safe->selectBand (b, ln); },
                [safe]            { if (safe != nullptr) safe->refreshAfterLaneEdit(); });
            m.addSubMenu ("Placement lanes", lanes);
            m.addSeparator();
            m.addItem (100, "Remove band");
            m.showMenuAsync (juce::PopupMenu::Options(), [safe, b, lane] (int r) {
                if (safe == nullptr || r <= 0 || r >= 1000) return;                     // ignore the lane rows' own ids
                if (r == 100)                                                            // remove the whole band (point off)
                {
                    safe->proc.beginHistoryGesture ("Remove Band " + juce::String (b + 1));   // one labelled step
                    safe->setParamGestured (tabby::bandId (b, "on"), 0.0);
                    safe->proc.endHistoryGesture();
                }
                else if (r <= 9)
                {
                    const int oldChoice = (int) std::lround (safe->proc.apvts.getRawParameterValue (tabby::bandId (b, "type"))->load());
                    safe->setParamGestured (safe->laneParamId (b, lane, "type"), r - 1);   // SHARED point type
                    tabby::snapQOnTypeSwitch (safe->proc.apvts, b, oldChoice, r - 1);      // untouched Q follows the new type's default
                }
            });
        }
        else          // empty: add a band here, if a slot is free
        {
            bool anyFree = false;
            for (int i = 0; i < tabby::kNumBands; ++i) if (! traces.param (i).on) { anyFree = true; break; }

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
    const auto selType = (selBand < 0) ? teq::FilterType::Bell : traces.param (selBand).type;   // shared point type
    if (selBand >= 0 && b < 0 && traces.param (selBand).on && whiskerRelevant (selType))
    {
        const auto wk = whiskerEnds (selBand, selLane);
        const bool onRight = e.position.getDistanceFrom (wk.second) < kNodeR + eqview::handles::kWhiskerGrabPad;
        const bool onLeft  = e.position.getDistanceFrom (wk.first)  < kNodeR + eqview::handles::kWhiskerGrabPad;
        if (onLeft || onRight)
        {
            draggingBand = selBand; draggingLane = selLane; draggingQ = true; qDragSide = onRight ? 1 : -1;
            whiskerSlope = slopeWhisker (selType);   // HP/LP + Notch step the discrete slope; others set Q
            if (auto* prm = proc.apvts.getParameter (laneParamId (selBand, selLane, whiskerSlope ? "slope" : "q")))
                prm->beginChangeGesture();
            // The whole whisker drag = ONE undo step (closed in endDragGesture).
            proc.beginHistoryGesture ((whiskerSlope ? "Slope Band " : "Q Band ") + juce::String (selBand + 1));
            lastDragFreq = (float) traces.param (selBand).lanes[(size_t) selLane].freq;
            grabKeyboardFocus();
            if (e.mods.isAltDown()) driveAudition (true, lastDragFreq, audQSetting);
            return;
        }
    }

    // Coincident-node cycling: collect every node within the grab radius; a repeated click IN PLACE while a
    // stacked node is already selected advances to the next one, otherwise the nearest wins.
    int pb = b, pl = lane;
    if (b >= 0)
    {
        Hit stack[tabby::kNumBands * teq::kNumLanes];
        const int nh = collectNodesAt (e.position, stack, tabby::kNumBands * teq::kNumLanes);
        if (nh > 1)
        {
            int curIdx = -1;
            for (int i = 0; i < nh; ++i) if (stack[i].band == selBand && stack[i].lane == selLane) curIdx = i;
            const bool inPlace = e.position.getDistanceFrom (lastClickPos) < kNodeR;
            if (inPlace && curIdx >= 0) { pb = stack[(curIdx + 1) % nh].band; pl = stack[(curIdx + 1) % nh].lane; }
        }
    }
    lastClickPos = e.position;

    // select the clicked node/lane (or clear on empty) so the toolbar follows, then start dragging
    selectBand (pb, pl);
    draggingBand = pb; draggingLane = pl;
    if (pb >= 0)
    {
        const auto& lp = traces.param (pb).lanes[(size_t) pl];
        const auto  lt = traces.param (pb).type;                                    // shared point type
        pressBand = pb; pressPos = e.position; pressMs = juce::Time::getMillisecondCounter(); pressMoved = false;
        // The whole node drag = ONE undo step — the killer use case the history gestures exist for
        // (grab = begin, release = end in endDragGesture; link mirrors fold in via the bracket drain).
        proc.beginHistoryGesture ("Move Band " + juce::String (pb + 1));
        if (auto* fp = proc.apvts.getParameter (laneParamId (pb, pl, "freq"))) fp->beginChangeGesture();
        draggingGain = hasGain (lt);
        if (draggingGain)
        {
            if (auto* gp = proc.apvts.getParameter (laneParamId (pb, pl, "gain"))) gp->beginChangeGesture();
            gainDragRefY    = (double) e.position.y;                              // relative-drag anchor...
            gainDragRefGain = lp.gainDb;                                          // ...at the node's current gain
        }
        lastDragFreq = (float) lp.freq;
        grabKeyboardFocus();
        if (e.mods.isAltDown()) driveAudition (true, lastDragFreq, audQSetting);
    }
}

void EqCurveDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if ((draggingBand >= 0 || placing) && e.source.getIndex() != dragSourceIndex)
        return;   // only the source that grabbed may move the drag (multi-touch)

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
    const double laneF0 = traces.param (draggingBand).lanes[(size_t) draggingLane].freq;   // the dragged lane
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
            setParam (laneParamId (draggingBand, draggingLane, "slope"), (double) slopeIndexForBw (bw));
        else                // others -> continuous Q (calibrated, hits 40 / 0.1 at the ends)
            setParam (laneParamId (draggingBand, draggingLane, "q"), juce::jlimit (0.1, 40.0, whiskerQForBw (bw)));
        positionToolbar();   // follow the node while dragging it by mouse
        return;
    }
    setParam (laneParamId (draggingBand, draggingLane, "freq"), xToFreq (e.position.x));
    if (draggingGain && ! lockY)   // latched at mouseDown; Alt+lock sweeps frequency only (gain frozen)
    {
        // Relative gain drag from an anchor (set at mouseDown, re-based on auto-zoom). Absolute mapping used to
        // snap the gain to the cursor, which — at the physical edge — read as the NEW scale's max and cascaded
        // ±3→30 instantly. Relative + re-anchor keeps the node's gain across a zoom.
        const double sens = 2.0 * gainRange / (double) juce::jmax (1, plotBottomY());   // dB per pixel at this scale
        const double g = juce::jlimit (-kGainMax, kGainMax, gainDragRefGain + sens * (gainDragRefY - (double) e.position.y));
        setParam (laneParamId (draggingBand, draggingLane, "gain"), g);
        // At the visible edge, widen ONE step (rate-limited) and KEEP g: the node lands at its value on the new
        // scale (e.g. +3 → mid-upper on ±6), not at the edge. Re-anchor so the cursor no longer reads as the new
        // max → no cascade; a further deliberate drag escalates the next step, up to ±30 where ±24 always fits.
        const juce::uint32 now = juce::Time::getMillisecondCounter();
        if (std::abs (g) >= gainRange - 0.05 && gainRange < kGainSteps[3] && now - lastZoomMs > 140)
        {
            lastZoomMs = now;
            setGainRange (nextGainStep (gainRange));
            gainDragRefGain = g;                    // keep the gain...
            gainDragRefY    = (double) e.position.y; // ...and re-base the anchor at the cursor on the new scale
        }
    }
    positionToolbar();   // follow the node while dragging it by mouse (cursor is on the node, not the bar)
}

void EqCurveDisplay::mouseUp (const juce::MouseEvent& e)
{
    if ((draggingBand >= 0 || placing) && e.source.getIndex() != dragSourceIndex)
        return;   // a second source's release must not end (or commit) another source's drag

    driveAudition (false);   // stop any drag-audition
    if (placing)   // release inside -> commit the previewed band; outside -> cancel
    {
        if (getLocalBounds().toFloat().contains (e.position))
        {
            if (placeSpec.hasGainCtrl && (placeGainFromDrag || ! placeMoved))
                addBandOfType (placeSpec.typeIndex, placePos, placeSpec.slopeIndex);                              // gain from the cursor (plain click or free drag)
            else
                addBandOfType (placeSpec.typeIndex, { placePos.x, dbToY (placeSpec.gainDb) }, placeSpec.slopeIndex);  // filter, or Alt-locked default gain
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

    // Selection / edit hotkeys are inert while ANY history gesture is open (a node/whisker drag
    // here, or a slider-bar drag anywhere): stepping the selection or nudging params would fold
    // foreign edits into the open gesture's snapshot, and Delete mid-drag would nest "Remove Band"
    // inside a step labelled "Move Band" (crew round).
    const bool midDrag = isDragActive() || proc.historyGestureOpen();

    // Alt + Left/Right steps the band selection (and picks the first band if none is selected yet).
    if (alt && code == juce::KeyPress::leftKey)  { if (! midDrag) stepSelection (-1); return true; }
    if (alt && code == juce::KeyPress::rightKey) { if (! midDrag) stepSelection (+1); return true; }

    if (selBand >= 0 && selBand < tabby::kNumBands)   // hotkeys for the selected node
    {
        if (midDrag)
            return true;   // every selected-node hotkey (arrows, Alt+Up/Down, Delete) is inert mid-gesture

        if (code == juce::KeyPress::backspaceKey || code == juce::KeyPress::deleteKey)
        {
            proc.beginHistoryGesture ("Remove Band " + juce::String (selBand + 1));   // one labelled step
            setParamGestured (tabby::bandId (selBand, "on"), 0.0);   // remove the band (free the slot)
            proc.endHistoryGesture();
            selectBand (-1);
            return true;
        }

        refreshDesigns();
        const auto&  lp = traces.param (selBand).lanes[(size_t) selLane];   // the focused lane
        const auto   lt = traces.param (selBand).type;     // shared point type
        const double lf = lp.freq;
        const double lg = lp.gainDb;
        const double lq = lp.Q;
        const int    ls = lp.slope;

        if (! alt)
        {
            const double fStep = std::exp2 (1.0 / 24.0);                                // freq nudge: a quarter-tone
            if (code == juce::KeyPress::leftKey)  { setParamGestured (laneParamId (selBand, selLane, "freq"), juce::jlimit (kFreqMin, kFreqMax, lf / fStep)); positionToolbar(); return true; }
            if (code == juce::KeyPress::rightKey) { setParamGestured (laneParamId (selBand, selLane, "freq"), juce::jlimit (kFreqMin, kFreqMax, lf * fStep)); positionToolbar(); return true; }
            if ((code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) && hasGain (lt))
            {
                setParamGestured (laneParamId (selBand, selLane, "gain"),
                                  juce::jlimit (-kGainMax, kGainMax, lg + (code == juce::KeyPress::upKey ? 0.5 : -0.5)));
                positionToolbar();
                return true;                                                            // gain +/- 0.5 dB
            }
        }
        else   // Alt + Up/Down -> Q (or slope for HP/LP);  Alt+Left/Right handled above
        {
            if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
            {
                const int d = (code == juce::KeyPress::upKey) ? +1 : -1;
                if (slopeWhisker (lt))                                                  // HP/LP + Notch -> step the slope (octaves)
                    setParamGestured (laneParamId (selBand, selLane, "slope"), (double) juce::jlimit (0, 6, slopeIndexFromDb ((int) ls) + d));
                else                                                                    // others -> Q +/- 0.1
                    setParamGestured (laneParamId (selBand, selLane, "q"), juce::jlimit (0.05, 40.0, lq + 0.1 * d));
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
    if (hoverPos.x > freqToX (kFreqPlaceMax))              return { -1.0f, -1.0f };   // no add in the 20k–28k display-only headroom

    // keep the "+" clear of anything grabbable: a margin around every node (all lanes) so reaching for a node
    // never pops the add-affordance under the cursor.
    for (int b = 0; b < tabby::kNumBands; ++b) if (traces.param (b).on)
        for (int L = 0; L < teq::kNumLanes; ++L)
            if (laneOn (b, L) && hoverPos.getDistanceFrom (nodePos (b, L)) < kNodeR + eqview::handles::kAddClearPad) return { -1.0f, -1.0f };

    // don't surface "+" over the selected lane's whisker bar (so you can grab a handle)
    const auto selType = (selBand >= 0) ? traces.param (selBand).type : teq::FilterType::Bell;   // shared point type
    if (selBand >= 0 && traces.param (selBand).on && whiskerRelevant (selType))
    {
        const auto wk = whiskerEnds (selBand, selLane);
        const float ny = nodePos (selBand, selLane).y;
        if (std::abs (hoverPos.y - ny) < 12.0f && hoverPos.x > wk.first.x - 8.0f && hoverPos.x < wk.second.x + 8.0f)
            return { -1.0f, -1.0f };
    }

    bool anyFree = false;
    for (int i = 0; i < tabby::kNumBands; ++i) if (! traces.param (i).on) { anyFree = true; break; }
    if (! anyFree)                                        return { -1.0f, -1.0f };   // all bands used

    return hoverPos;   // "+" follows the cursor across the WHOLE graph (min→max); predictAdd + the preview read its X/Y
}

void EqCurveDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    refreshDesigns();
    const Hit hit = nodeAt (e.position);
    if (hit.band >= 0)                                            // on a node -> toggle that lane's bypass (ghost)
    {
        const bool byp = traces.param (hit.band).lanes[(size_t) hit.lane].bypass;
        setParamGestured (laneParamId (hit.band, hit.lane, "bypass"), byp ? 0.0 : 1.0);
        selectBand (hit.band, hit.lane);
    }
    else
        smartAdd (e.position);                                   // empty -> add (edge -> HP/LP, else Bell)
}

void EqCurveDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    refreshDesigns();
    Hit hit = draggingBand >= 0 ? Hit { draggingBand, draggingLane } : nodeAt (e.position);
    if (hit.band < 0) hit = { selBand, selLane };      // not over a node -> the SELECTED one (a narrow node
    if (hit.band < 0) return;                          // — BP on the zero line — is a tiny wheel target)
    auto* qp = proc.apvts.getParameter (laneParamId (hit.band, hit.lane, "q"));
    if (qp == nullptr) return;
    const double cur    = qp->convertFrom0to1 (qp->getValue());   // live value (not the timer cache)
    const double factor = wheel.deltaY > 0.0f ? 1.15 : 1.0 / 1.15;
    setParamGestured (laneParamId (hit.band, hit.lane, "q"), juce::jlimit (0.05, 40.0, cur * factor));
}
