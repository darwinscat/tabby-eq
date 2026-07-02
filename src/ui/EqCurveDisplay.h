// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <teq/EqBand.h>
#include <teq/SpectrumTap.h>
#include <felitronics/core/Fft.h>

#include "PluginProcessor.h"

#include <array>

//==============================================================================
// The classic EQ canvas — log-frequency / dB grid, a live post spectrum (FFT of the engine's
// output tap), the composite response curve (computed race-free from the param snapshot), and a
// draggable node per active band. This is the foundation the semantic layer (slice 3+) sits on.
class EqCurveDisplay : public juce::Component, private juce::Timer
{
public:
    explicit EqCurveDisplay (TabbyEqAudioProcessor& p);
    ~EqCurveDisplay() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // The floating per-band toolbar (owned by the editor) is parented here so it overlays the canvas
    // and tracks the selected node. Pass nullptr to detach.
    void setToolbar (juce::Component* t) noexcept;
    void stepSelection (int dir);   // select the prev/next active band (frequency order, wraps)
    void clearSelection();          // deselect (hides the floating toolbar)
    void refreshToolbar();          // re-place the toolbar at the selected node (after a window edit)
    void setSelectedSide (bool side);   // the toolbar switched the Mid/Side lane -> highlight that node

    void mouseDown        (const juce::MouseEvent&) override;
    void mouseDrag        (const juce::MouseEvent&) override;
    void mouseUp          (const juce::MouseEvent&) override;
    void mouseMove        (const juce::MouseEvent&) override;
    void mouseExit        (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove   (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed       (const juce::KeyPress&) override;   // Esc cancels an in-progress add-drag
    void modifierKeysChanged (const juce::ModifierKeys&) override;   // crisp Alt-audition toggle mid-drag

    // Set by the editor: fires when the selected band changes (-1 = none). Drives the edit strip.
    std::function<void(int, bool)> onBandSelected;   // (band, side-lane); -1 = none
    std::function<void()>    onToggleFullscreen;   // 'f' pressed — editor toggles real fullscreen
    int  selectedBand() const noexcept { return selBand; }
    void setAnalyzerPre (bool pre) noexcept { analyzerPre = pre; }   // analyzer reads pre- or post-EQ
    void setViewBandColors (bool v) noexcept { perBandColors = v; repaint(); }
    void setViewBandCurves (bool v) noexcept { perBandCurves = v; repaint(); }
    void setViewBandFill   (bool v) noexcept { perBandFill = v; repaint(); }
    void setViewLongSolo   (bool v) noexcept { longPressSolo = v; }
    bool viewBandColors() const noexcept { return perBandColors; }
    bool viewBandCurves() const noexcept { return perBandCurves; }
    bool viewBandFill()   const noexcept { return perBandFill; }
    bool viewLongSolo()   const noexcept { return longPressSolo; }
    void setAddLineMode (int m) noexcept { addLine = (AddLine) juce::jlimit (0, 3, m); repaint(); }   // 0 off /1 zero /2 curve /3 both
    int  addLineMode() const noexcept { return (int) addLine; }
    void  setAuditionQ (float q) noexcept { audQSetting = juce::jlimit (0.5f, 18.0f, q); }            // Alt-drag listen width
    float auditionQ() const noexcept { return audQSetting; }
    void  setAuditionVisual (int v) noexcept { audVisual = (AudVisual) juce::jlimit (0, 1, v); repaint(); }   // 0 spotlight /1 bell
    int   auditionVisual() const noexcept { return (int) audVisual; }
    void  setAuditionLockGain (bool v) noexcept { audLockGain = v; }   // Alt-drag changes only freq (sweep)
    bool  auditionLockGain() const noexcept { return audLockGain; }

    // Floating-toolbar placement strategy (View menu; persisted). 0 Classic (centered above/below — the
    // original) · 1 Anchor-to-open-side · 2 Collision-aware (+leader line) · 3 Hybrid (float, else dock to an
    // edge when the graph is crowded). (In the design discussion these were labelled 0 / 1 / 2 / 7.)
    void setToolbarPlacement (int m) noexcept { toolbarPlace = (ToolbarPlace) juce::jlimit (0, 4, m); lastPlaceSlot = -1; positionToolbar(); repaint(); }
    int  toolbarPlacement() const noexcept { return (int) toolbarPlace; }

private:
    void timerCallback() override;

    // coordinate maps
    float  freqToX (double f) const noexcept;
    double xToFreq (float x)  const noexcept;
    float  dbToY   (double db) const noexcept;     // curve gain scale (± kGainRange around centre)
    double yToDb   (float y)   const noexcept;
    float  specDbToY (double db) const noexcept;   // spectrum dBFS scale (always full height)
    int    plotBottomY() const noexcept;           // bottom of the curve/node area — above the Fixed-lane strip

    struct Hit { int band = -1; bool side = false; };   // a node hit: which band + which lane (M/S)

    void   refreshDesigns();                       // pull the bands into the cache + design both lanes
    // The curve is DESIGNED + evaluated at an oversampled "display" rate (>= 96k), not the real fs. A matched
    // biquad's magnitude is even about fs/2, so at low real rates (44.1/48k) an LP/BP would visibly FLATTEN
    // as it nears Nyquist (slope -> 0) and then kink; oversampling pushes that flattening far past the 28k
    // axis so the curve shows the smooth analog INTENT (FabFilter-style) and looks identical at every rate.
    // (The spectrum analyzer still uses the real fsCache — that's true measured content, not a design.)
    double designFs() const noexcept { return juce::jmax (fsCache, 96000.0); }

    // Two-lane UX helpers: a "point" is unsplit (single ST node) or split (Mid + Side nodes). The `side`
    // bool identifies a node — false = the ST/Mid node, true = the Side node. laneOf maps (band, side) to a
    // teq::Lane; splitB reads the paint cache, splitLive reads the live atomics (async menu callbacks).
    bool       splitB    (int b) const noexcept;            // paramCache: is the point split (Mid or Side lane on)?
    bool       splitLive (int b) const noexcept;            // live atomics variant
    bool       laneEnabled (int b, bool side) const noexcept;   // is that node's lane ON? Migration can make {m}-only /
                                                                // {s}-only points — one node each, never a phantom
    teq::Lane  laneOf    (int b, bool side) const noexcept; // (band, side) -> ST / Mid / Side (from the cache)
    teq::Lane  laneOfLive(int b, bool side) const noexcept; // live-atomics variant
    static const char* laneKeyStr (teq::Lane l) noexcept;   // "st" / "l" / "r" / "m" / "s"
    juce::String laneParamId (int b, bool side, juce::StringRef base) const;   // node -> APVTS id (type=shared, bypass=lane byp)
    double compositeDb (double f, bool side = false) const noexcept;   // Mid (side=false) or Side composite
    juce::Point<float> nodePos (int band, bool side = false) const noexcept;
    std::pair<juce::Point<float>, juce::Point<float>> whiskerEnds (int b, bool side = false) const noexcept;
    juce::Colour bandColour (int b) const noexcept;    // per-band (or per-type) colour
    double       bandDb (int b, double f, bool side = false) const noexcept;   // one lane's response (dB)
    juce::Point<float> addButtonAt() const noexcept;   // the "+" on the curve under the cursor, or {-1,-1}
    Hit    nodeAt (juce::Point<float> p) const noexcept;   // band + lane under p (band = -1 if none)
    void   setParam (const juce::String& id, double value);
    void   setParamGestured (const juce::String& id, double value);   // begin+set+end (one-shot UI edits)
    void   endDragGesture();   // balance any open begin/endChangeGesture — from mouseUp AND the dtor
    void   addBandOfType (int typeIndex, juce::Point<float> at, int slopeIndex = -1);   // enable the first free band
    void   smartAdd (juce::Point<float> at);   // add with a smart default type (grid 3x2 -> see predictAdd)

    // Smart add default: which type/slope/gain a click at `at` produces (grid: 3 freq-thirds x
    // above/below 0 dB). Single source of truth for the action AND the "+" ghost preview.
    enum class AddLine { Off, ZeroLine, Curve, Both };
    struct AddSpec { int typeIndex = 0; int slopeIndex = -1; double gainDb = 2.0; bool hasGainCtrl = true; };
    AddSpec predictAdd (juce::Point<float> at) const noexcept;
    void    drawAddPreview (juce::Graphics&, const AddSpec&, juce::Point<float> at, bool dragging) const;
    void    drawListenVisual (juce::Graphics&, double freqHz, double q, const juce::String& label) const;   // bell/spotlight + beam, shared by audition & solo
    void    driveAudition (bool on, float freqHz = 1000.0f, float q = 6.0f);   // proc listen + spotlight state
    void   pushSpectrum();
    void   selectBand (int newSel, bool side = false);   // update selection + fire onBandSelected
    void   positionToolbar();                        // float the toolbar near the selected node (per toolbarPlace)
    juce::Rectangle<int> placeClassic    (juce::Point<float> node) const noexcept;   // 0: centered above/below + clamp
    juce::Rectangle<int> placeAnchorSide (juce::Point<float> node) const noexcept;   // 1: extend into the open side
    juce::Rectangle<int> bestFloatCandidate (juce::Point<float> node, int& occlOut, int& slotOut) const noexcept;   // 2/3 core
    int    stripMaxY() const noexcept;                     // lowest toolbar top that keeps the bottom freq axis clear
    void   setGainRange (double r, bool persist = true);   // set the visible dB scale + sync combo/state
    static double nextGainStep (double r) noexcept;        // next larger step in kGainSteps (clamps at the max)
    int    gainStepIndex() const noexcept;                 // nearest kGainSteps index for the current gainRange
    juce::String readoutText (int b) const;          // "1.24 kHz  +3.5 dB  Q 2.0" for the node bubble
    void   buildSpectrumPaths (juce::Path& fillOut, juce::Path& peakOut, float w, float h) const;  // liquid + peak-hold

    TabbyEqAudioProcessor& proc;

    // analyzer (core::Fft seam — JUCE-free; layout [DC, Nyquist, re1,im1, …])
    felitronics::core::fft::DefaultRealFft fft;
    std::array<float, teq::kSpectrumFftSize>     window {};
    std::array<float, teq::kSpectrumFftSize * 2> fftBuf {};
    std::array<float, teq::kSpectrumFftSize>     spec {};
    std::array<float, teq::kSpectrumFftSize / 2 + 1> specDb {};
    std::array<float, teq::kSpectrumFftSize / 2 + 1> specPeak {};   // slow-decay peak-hold

    // per-paint cache of the band designs (shared by curve / nodes / hit-test)
    teq::BandParams paramCache[tabby::kNumBands];
    teq::BandDesign designCache[tabby::kNumBands];       // Mid/main lane design
    teq::BandDesign designCacheSide[tabby::kNumBands];   // Side lane design (M/S bands only)
    double fsCache = 44100.0;

    int  draggingBand = -1;
    bool draggingSide = false;   // the node being dragged is the Side lane
    bool draggingGain = false;
    double gainDragRefY = 0.0, gainDragRefGain = 0.0;   // relative gain-drag anchor (re-based on auto-zoom)
    bool draggingQ    = false;   // dragging a Q-whisker handle (sets bandwidth, not freq/gain)
    int  qDragSide    = 0;       // which handle: +1 right / -1 left (clamps to its side, no crossing)
    bool whiskerSlope = false;   // the dragged whisker sets slope (HP/LP) rather than Q

    juce::uint32 pressMs = 0;     // long-press-to-solo: time / band / moved? / active? / prior solo
    int  pressBand     = -1;
    bool pressMoved    = false;
    bool momentarySolo = false;
    int  prevSoloBand  = -1;
    juce::Point<float> pressPos;
    int  selBand      = -1;      // currently selected band (highlighted; shown in the edit strip)
    bool selSide      = false;   // selected lane (false = Mid/main, true = Side) for M/S bands
    int  starveTicks  = 0;       // consecutive analyzer ticks with no new frame
    juce::Point<float> hoverPos { -1.0f, -1.0f };   // last mouse-move position (hover halo + "+")
    bool analyzerPre = false;                       // analyzer taps pre-EQ (true) or post-EQ (false)
    bool perBandColors = true;                      // each band a fixed distinct colour (else by type)
    bool perBandCurves = true;                      // draw each band's own faint colour curve
    bool perBandFill   = false;                     // fill under each band's curve (off by default)
    bool longPressSolo = true;                      // hold the mouse on a node to solo it

    AddLine addLine = AddLine::ZeroLine;             // which line surfaces "+" (View option) — default: the 0 dB line only
    bool    placing    = false;                      // press-drag add in progress
    bool    placeMoved = false;                      // moved enough to take the gain from the drag Y
    AddSpec placeSpec;                               // type/slope locked at press
    juce::Point<float> placeDown { -1.0f, -1.0f };   // press origin
    juce::Point<float> placePos  { -1.0f, -1.0f };   // current cursor during the place-drag

    bool  auditioning = false;                       // drag-audition active -> draw the listen spotlight
    float audFreq = 1000.0f, audQ = 6.0f;            // its centre / Q (for the spotlight width)
    float audQSetting = 6.0f;                         // configurable audition Q (View option)
    enum class AudVisual { Spotlight, Bell };
    AudVisual audVisual = AudVisual::Bell;            // how the audition is drawn (View option)
    juce::Component* toolbar = nullptr;               // floating per-band toolbar (owned by the editor)
    static constexpr int kToolbarW    = 256, kToolbarH = 64;   // fixed width; the [M][S] tabs' slot is always reserved
                                                               // (right of ST) so the strip never resizes on ST toggle
    static constexpr int kBottomAxisH = 16;           // freq-label strip at the very bottom the edit-strip must NOT cover
    static constexpr int kLaneH       = 84;           // Fixed-lane reserve = strip + freq axis (kToolbarH + kBottomAxisH + gap)

    // Toolbar placement strategy (see setToolbarPlacement). Classic = the original centered-above behaviour.
    enum class ToolbarPlace { Classic, AnchorSide, Collision, Hybrid, FixedLane };
    ToolbarPlace toolbarPlace = ToolbarPlace::Classic;
    int  lastPlaceSlot = -1;                          // hysteresis: last chosen candidate slot (collision/hybrid)
    bool showLeader = false;                          // draw the node<->toolbar connector (collision/hybrid)
    juce::Point<float> leaderNode, leaderBar;         // connector endpoints (canvas coords)

    // Vertical dB scale (top-right combo, persisted). gainRange is the VISIBLE ± dB window; it auto-zooms out
    // while dragging a node past the edge (kGainSteps), so a node never gets stranded under the floating strip.
    double gainRange = 12.0;                          // visible ± dB (curve y-axis); NOT the gain clamp (that's kGainMax)
    juce::uint32 lastZoomMs = 0;                      // rate-limit for drag auto-zoom (one step per interval)
    juce::ComboBox gainScaleCombo;                    // vertical-scale picker (±3/6/12/30 dB) — top-right overlay

    bool  audLockGain = true;                         // Alt-drag sweeps frequency only (gain frozen)
    float lastDragFreq = 1000.0f;                     // last audition centre (for crisp modifier toggling)
    bool  placeGainFromDrag = false;                  // press-drag add: take gain from drag Y (not default)

    static constexpr double kFreqMin   = 20.0, kFreqMax = 28000.0;   // AXIS range: top runs ~½-oct past 20k so the >20k rolloff is visible
    static constexpr double kFreqPlaceMax = 20000.0;                 // bands can only be PLACED up to here (= the freq param max); 20k–28k is display-only
    static constexpr double kGainMax   = 24.0;          // ± dB — the real gain-param clamp (drag / keyboard)
    static constexpr double kGainSteps[4] = { 3.0, 6.0, 12.0, 30.0 };   // selectable vertical-scale steps (visible ± dB)
    static constexpr double kSpecTop   = 6.0,  kSpecBottom = -90.0;
    static constexpr float  kNodeR     = 6.0f;
    static constexpr float  kAddThreshold = 36.0f;       // px from the curve to surface the "+" add button
    static constexpr float  kPeakFallDb = 0.8f;          // peak-hold fall (~24 dB/s at 30 Hz)
    static constexpr double kTiltDbPerOct = 4.5;         // analyzer tilt — pink-noise comp, like Pro-Q / Neutron
    static constexpr double kTiltPivotHz  = 1000.0;      // tilt pivot (highs lift, lows drop around 1 kHz)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqCurveDisplay)
};
