// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_dsp/juce_dsp.h>
#include <teq/EqBand.h>

#include <felitronics/analysis/PlotMap.h>
#include <felitronics/analysis/SpectrumPane.h>
#include <felitronics/analysis/MultiResSpectrumPane.h>
#include <functional>
#include <memory>

// PlotMap and SpectrumPane graduated from the eqview incubator to felitronics-core once OrbitAmp
// became their second consumer; the eqview names keep reading as before. MultiResSpectrumPane was
// born in core (docs/ANALYZER-MULTIRES.md there) — the constant-Q sibling of the classic pane.
namespace eqview
{
using felitronics::analysis::PlotMap;
using felitronics::analysis::SpectrumPane;             // the classic pane on the core's scalar reference FFT
using felitronics::analysis::MultiResSpectrumPane;     // the constant-Q pane on the same

// Both panes take their FFT as a parameter in core. The display talks to them through this seam so the
// backend is chosen at RUNTIME — the scalar reference, or, when the build carries TABBYEQ_WITH_PFFFT,
// pffft's SIMD transform in packed order (≈3× cheaper per tick, numerically equivalent: nulled against
// the scalar in the core's pffft suite). Beside, not instead: Analyzer → FFT switches, the audio path
// never changes. A box owns one pane on the heap; the display holds four (classic + multi, pre + post)
// and remakes them when the backend changes, so the first frame after a switch seeds cleanly.
struct PaneBox
{
    virtual ~PaneBox() = default;
    virtual float* frameInput() noexcept = 0;
    virtual void   ingest (int order) noexcept = 0;
    virtual void   starve() noexcept = 0;
    virtual void   reset() noexcept = 0;
    virtual void   setSpeed (float smoothCoeff, float peakFallDb) noexcept = 0;
    virtual void   setCover (int hopSamples) noexcept = 0;       // the multi pane's Welch cover; a no-op on the classic one
    virtual int    frameOrder() const noexcept = 0;              // multi: the tap order it needs; classic: −1 (any)
    virtual void   buildColumns (const PlotMap& pm, double fs, double tiltDbPerOct, double tiltPivotHz,
                                 const std::function<void (int, float, float, float)>& emit) const = 0;
};
std::unique_ptr<PaneBox> makeClassicPane (bool pffft);   // EqCurveDisplay.cpp; pffft falls back to scalar when not built in
std::unique_ptr<PaneBox> makeMultiPane   (bool pffft);
bool pffftAvailable() noexcept;                          // TABBYEQ_WITH_PFFFT at build time
}
#include "eqview/TraceSet.h"
#include "eqview/HandleMath.h"
#include "PluginProcessor.h"
#include "ui/Palette.h"

//==============================================================================
// A flat text overlay button — no frame, no background, no dropdown arrow. Used for the vertical-
// scale picker so it never boxes over the graph's axis; a click opens a plain PopupMenu.
class FlatScaleButton final : public juce::Button
{
public:
    FlatScaleButton() : juce::Button ({}) {}
    juce::String text;
    // The picker heads the orange EQ-gain column, so it wears the same hue — on a lifted plate, so
    // it reads as the one CLICKABLE thing among the painted numbers.
    void paintButton (juce::Graphics& g, bool highlighted, bool) override
    {
        g.setColour (tabby::palette::panel().withAlpha (highlighted ? 1.0f : 0.85f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (highlighted ? tabby::palette::orange().brighter (0.25f) : tabby::palette::orange());
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (text, getLocalBounds(), juce::Justification::centred);
    }
};

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
    void stepSelection (int dir);   // step across ALL visible nodes in global frequency order (wraps)
    void clearSelection();          // deselect (hides the floating toolbar)
    void refreshToolbar();          // re-place the toolbar at the selected node (after a window edit)
    void refreshAfterLaneEdit();    // lane set / links changed elsewhere -> re-cache + re-place + repaint
    void setSelectedLane (int lane);   // the toolbar switched the active lane -> highlight that node

    void mouseDown        (const juce::MouseEvent&) override;
    void mouseDrag        (const juce::MouseEvent&) override;
    void mouseUp          (const juce::MouseEvent&) override;
    void mouseMove        (const juce::MouseEvent&) override;
    void mouseExit        (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove   (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed       (const juce::KeyPress&) override;   // Esc cancels an in-progress add-drag
    void modifierKeysChanged (const juce::ModifierKeys&) override;   // crisp Alt-audition toggle mid-drag

    // A node/whisker drag (an open history gesture) or a "+"-placement drag is in flight — the
    // editor gates history NAVIGATION on this (undo/switch mid-drag is the engine's misuse path).
    bool isDragActive() const noexcept { return draggingBand >= 0 || placing; }

    // Set the visible dB scale + sync combo/state (public: the editor re-applies the snapshot's
    // saved scale after every history apply — see syncViewFromState).
    void setGainRange (double r, bool persist = true);

    // Balance any open begin/endChangeGesture + close the node drag's history gesture — from
    // mouseUp, this dtor, AND the editor's dtor (which must close the node drag BEFORE force-
    // closing leftover slider brackets, so nothing double-ends).
    void endDragGesture();

    // Set by the editor: fires when the selected band/lane changes (-1 = none). Drives the edit strip.
    std::function<void(int, int)> onBandSelected;   // (band, lane 0..4); band -1 = none
    std::function<void()>    onToggleFullscreen;   // 'f' pressed — editor toggles real fullscreen
    int  selectedBand() const noexcept { return selBand; }
    // Analyzer view state (driven by the bottom-bar popover): which taps are drawn (pre = dimmed
    // grey behind the coloured post), freeze (hold the current spectra), display tilt and range.
    void setAnalyzerShow (bool pre, bool post) noexcept { showAnaPre = pre; showAnaPost = post; repaint(); }
    void setAnalyzerFrozen (bool f) noexcept            { anaFrozen = f; }
    void setAnalyzerTilt   (double dbPerOct) noexcept   { anaTilt = dbPerOct; repaint(); }
    void setAnalyzerRange  (double dB) noexcept         { anaRange = juce::jlimit (30.0, 200.0, dB); repaint(); }
    void setAnalyzerSpeed  (int preset) noexcept;       // 0 Slow / 1 Medium / 2 Fast — every pane
    // Resolution mode: the constant-Q MULTI-resolution pane (several FFT lengths at once, stitched by
    // frequency — the lows read, the highs don't smear) vs the classic fixed-size pane. The multi pane
    // asks the tap for its longest frame; the editor pushes that order to the processor.
    void setAnalyzerMulti  (bool on);
    bool analyzerMulti()     const noexcept { return anaMulti; }
    int  multiFrameOrder()   const noexcept { return multiPost->frameOrder(); }
    // FFT backend of every pane: the scalar reference or pffft (only when built in — see eqview::pffftAvailable).
    void setAnalyzerFft    (bool pffft);
    bool analyzerPffft()     const noexcept { return anaPffft; }
    bool analyzerPreShown()  const noexcept { return showAnaPre; }
    bool analyzerPostShown() const noexcept { return showAnaPost; }
    bool analyzerFrozen()    const noexcept { return anaFrozen; }
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

    // Node<->strip leader line (View menu; persisted). Governs EVERY placement mode uniformly:
    // 0 Off (never) · 1 Flash (~1 s fade-out on selection change / strip re-anchor — the default) ·
    // 2 Always. Collision/Hybrid's previous built-in always-on leader is now just the Always setting.
    void setLeaderMode (int m) noexcept { leaderOpt = (Leader) juce::jlimit (0, 2, m); repaint(); }
    int  leaderMode() const noexcept { return (int) leaderOpt; }

private:
    void timerCallback() override;

    // coordinate maps — thin wrappers over eqview::PlotMap (the single, unit-tested home of the
    // mapping math); plotMap() snapshots the component's live geometry into one.
    eqview::PlotMap plotMap() const noexcept;
    float  freqToX (double f) const noexcept;
    double xToFreq (float x)  const noexcept;
    float  dbToY   (double db) const noexcept;     // curve gain scale (± kGainRange around centre)
    double yToDb   (float y)   const noexcept;
    float  specDbToY (double db) const noexcept;   // spectrum dBFS scale (always full height)
    int    plotBottomY() const noexcept;           // bottom of the curve/node area — above the Fixed-lane strip

    struct Hit { int band = -1; int lane = 0; };   // a node hit: which band + which placement lane (0..4)

    void   refreshDesigns();                       // pull the bands into the cache + design every enabled lane
    // Oversampled "display" design rate (>= 96k) — the rule and its rationale live in
    // eqview::TraceSet (the analyzer still uses the real sample rate: measured content, not a design).
    double designFs() const noexcept { return traces.designFs(); }

    // Placement-lane node helpers. A "point" has a node per ENABLED lane (ST/L/R/M/S). `lane` is a teq::Lane
    // index 0..4. laneOn reads the paint cache; laneOnLive reads the live atomics (async menu callbacks).
    // A point is UNSPLIT (looks as today: per-band colour, no badge) when < 2 lanes are enabled; ≥ 2 → each
    // node takes its lane colour + a permanent badge.
    bool       laneOn     (int b, int lane) const noexcept;   // cache: is that lane enabled on the point?
    bool       laneOnLive (int b, int lane) const noexcept;   // live-atomics variant
    int        laneCount  (int b) const noexcept;             // # enabled lanes (cache)
    bool       multiLane  (int b) const noexcept;             // ≥ 2 enabled lanes -> lane colours + badges
    static teq::Axis   axisForLane (int lane) noexcept;       // lane -> the display axis it rides (ST -> the Mid hero)
    static const char* laneKeyStr  (int lane) noexcept;       // "st" / "l" / "r" / "m" / "s"
    juce::String laneParamId (int b, int lane, juce::StringRef base) const;   // node -> APVTS id (type/on = shared)
    double compositeDbAxis (double f, teq::Axis a) const noexcept;   // one stereo axis composite (dB), from the cache
    juce::Point<float> nodePos (int band, int lane) const noexcept;
    std::pair<juce::Point<float>, juce::Point<float>> whiskerEnds (int b, int lane) const noexcept;

    // --- point dynamics (DYNAMICS.md § 8) ------------------------------------------------------------
    // The dynamic handle hangs off the node at the far end of the range; with no range yet it rests one
    // grip-length below, which is the affordance the whole feature is dragged out of. Affordances exist
    // only for gain-bearing types (a notch or an all-pass has no gain to modulate) and only on a node the
    // user is already looking at — selected or hovered — unless the point already carries a range, in
    // which case the band is permanent state and always drawn.
    bool  dynRelevant  (int b) const noexcept;                       // the point's type can carry dynamics at all
    // `hoverHit` is passed in rather than looked up inside: this runs per node inside paint, and a
    // nodeAt() per node would rescan every node — 120 × 120 distance tests a frame.
    bool  dynShowsGrip (int b, int lane, Hit hoverHit) const noexcept;   // draw/hit the grab handle right now
    float dynHandleY   (int b, int lane) const noexcept;             // y of the handle (range end, or the resting grip)
    double dynLiveDeltaDb (int b, int lane) const noexcept;          // the published GR for this lane (0 when idle)
    // The handle under a point, or band -1. ONE home for the grab geometry: mouseDown grabs by it and
    // addButtonAt() suppresses the "+" over it, so the two can never disagree about where the handle is.
    int   dynHandleAt (juce::Point<float> p, int& laneOut) const noexcept;
    juce::Colour bandColour (int b) const noexcept;    // per-band (or per-type) colour
    juce::Colour nodeColour (int b, int lane) const noexcept;   // lane colour for split points, else per-band
    double       bandDb (int b, double f, int lane) const noexcept;   // one lane's response (dB)
    juce::Point<float> addButtonAt() const noexcept;   // the "+" on the curve under the cursor, or {-1,-1}
    Hit    nodeAt (juce::Point<float> p) const noexcept;   // nearest node under p (band = -1 if none)
    int    collectNodesAt (juce::Point<float> p, Hit* out, int maxOut) const noexcept;   // all nodes within grab radius
    void   setParam (const juce::String& id, double value);
    void   setParamGestured (const juce::String& id, double value);   // begin+set+end (one-shot UI edits)
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
    void   selectBand (int newSel, int lane = 0);   // update selection + fire onBandSelected
    void   positionToolbar();                        // float the toolbar near the selected node (per toolbarPlace)
    juce::Rectangle<int> placeClassic    (juce::Point<float> node) const noexcept;   // 0: centered above/below + clamp
    juce::Rectangle<int> placeAnchorSide (juce::Point<float> node) const noexcept;   // 1: extend into the open side
    juce::Rectangle<int> bestFloatCandidate (juce::Point<float> node, int& occlOut, int& slotOut) const noexcept;   // 2/3 core
    // The strip's LIVE size — it grows sideways when the dynamics panel is disclosed, so placement must
    // ask the component rather than trust the constants (which are only the collapsed defaults).
    int    toolbarH() const noexcept { return toolbar != nullptr && toolbar->getHeight() > 0 ? toolbar->getHeight() : kToolbarH; }
    int    toolbarW() const noexcept { return toolbar != nullptr && toolbar->getWidth()  > 0 ? toolbar->getWidth()  : kToolbarW; }
    int    stripMaxY() const noexcept;                     // lowest toolbar top that keeps the bottom freq axis clear
    static double nextGainStep (double r) noexcept;        // next larger step in kGainSteps (clamps at the max)
    int    gainStepIndex() const noexcept;                 // nearest kGainSteps index for the current gainRange
    void   buildSpectrumPaths (const eqview::PaneBox& pane, juce::Path& fillOut, juce::Path& peakOut) const;   // fill + peak-hold paths from any pane (geometry from plotMap())
    void   makePanes();                                                                                       // (re)create the four boxes on the current backend

    TabbyEqAudioProcessor& proc;

    // analyzer — the JUCE-free spectrum pipelines (window/FFT/smoothing/peak-hold/columns) live in
    // felitronics-core's panes (unit-tested there); this component owns the frame source + path assembly.
    // Four boxes on the heap (the multi pane is ~0.8 MB): classic post/pre and multi-res post/pre, on
    // whichever FFT backend Analyzer → FFT selected (see eqview::PaneBox).
    std::unique_ptr<eqview::PaneBox> analyzer, analyzerPrePane;   // classic pane: post (coloured) / pre (dimmed, behind)
    std::unique_ptr<eqview::PaneBox> multiPost, multiPre;         // constant-Q multi-resolution pane: post / pre
    std::array<float, eqview::SpectrumPane::kMaxSize> tapDrain {};   // discard buffer for HIDDEN taps (max window; see pushSpectrum)
    bool  anaMulti = true;                          // which pane pair is fed and drawn (Analyzer → Resolution)
    bool  anaPffft = false;                         // which FFT the boxes were made with (Analyzer → FFT)
    float anaSmooth = 0.25f, anaFall = 0.8f;        // the speed preset in effect (re-applied when boxes are remade)

    // traces — the response-curve calculator (param snapshot + per-lane designs + dB evaluation)
    // lives in eqview::TraceSet (unit-tested); shared by curve / nodes / hit-test via param()/design().
    eqview::TraceSet traces;

    int  draggingBand = -1;
    int  dragSourceIndex = -1;   // the input source that owns the current drag/placement (multi-touch filter)
    int  draggingLane = 0;       // the placement lane of the node being dragged (0..4)
    bool draggingGain = false;
    double gainDragRefY = 0.0, gainDragRefGain = 0.0;   // relative gain-drag anchor (re-based on auto-zoom)
    bool draggingQ    = false;   // dragging a Q-whisker handle (sets bandwidth, not freq/gain)
    bool draggingRange = false;  // dragging the dynamic handle (sets dyn range, and arms dynamics)
    int  qDragSide    = 0;       // which handle: +1 right / -1 left (clamps to its side, no crossing)
    bool whiskerSlope = false;   // the dragged whisker sets slope (HP/LP) rather than Q

    juce::uint32 pressMs = 0;     // long-press-to-solo: time / band / moved? / active? / prior solo
    int  pressBand     = -1;
    bool pressMoved    = false;
    bool momentarySolo = false;
    int  prevSoloBand  = -1;
    juce::Point<float> pressPos;
    int  selBand      = -1;      // currently selected band (highlighted; shown in the edit strip)
    int  selLane      = 0;       // selected placement lane (0..4) of the selected band
    juce::Point<float> lastClickPos { -1.0e9f, -1.0e9f };   // coincident-node cycling: a repeat click in place cycles
    int  justPlacedBand  = -1;   // band the LAST click's "+" placement created, and when — a double-click
    juce::uint32 justPlacedMs = 0;   // must not bypass the node its own first click just gave birth to
    juce::Point<float> hoverPos { -1.0f, -1.0f };   // last mouse-move position (hover halo + "+")
    bool   showAnaPre  = false;                     // draw the pre-EQ spectrum (dimmed, behind)
    bool   showAnaPost = true;                      // draw the post-EQ spectrum (coloured)
    bool   anaFrozen   = false;                     // hold the panes (no pulls -> spectra stand still)
    double anaTilt     = kTiltDbPerOct;             // display tilt, dB/oct around kTiltPivotHz
    double anaRange    = -kSpecBottom;              // spectrum RANGE preset: the floor is -range dBFS
                                                    // (default 90 == the classic -90 floor exactly)
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
    static constexpr int kToolbarW    = 234, kToolbarH = 64;   // COLLAPSED size = EXACTLY the strip's top-row buttons +
                                                               // margins + the DYN rail (see BandEditStrip::resized).
                                                               // The HEIGHT never changes; the WIDTH grows when the
                                                               // dynamics panel is disclosed, which is why placement
                                                               // reads toolbarW()/toolbarH(), not these
    static constexpr int kBottomAxisH = 16;           // freq-label strip at the very bottom the edit-strip must NOT cover
    // Right gutter reserved for the two scales — a CONSTANT number of pixels, never a fraction of
    // the width. The plot (grid, curves, spectrum, fog, the Nyquist hairline) stops at its left
    // edge, so the scales always stand in clean background: anything width-proportional — Nyquist
    // above all, which sits at ~0.98·plotWidth — would otherwise land dead on the numbers on a
    // narrow window and clear them on a wide one.
    //
    // Two scales, two hues — you never have to work out which number belongs to which curve — and
    // ONE vertical rule between them: the plot's right edge, drawn violet. The EQ GAIN scale
    // (orange, under the range picker) OVERLAYS the plot's right end, inside that rule; only the
    // ANALYZER LEVEL scale (dim violet, every 10 dBFS) lives outside it, so the gutter only has to
    // be as wide as one column.
    static constexpr int kScaleGutterW = 32;
    // The two number columns straddle the plot's edge, and nothing is drawn between them any more —
    // so the AIR between them is what separates them: the gain numbers end ON the edge, the level
    // numbers start 4 px outside it. Their own alignment does the rest — the level column is
    // right-aligned in a box just wide enough for "-100" (24 px cut it to "-1…" on a deep range),
    // so every label shorter than that leaves far more room than the boxes suggest.
    static constexpr int kGainColW = 24, kGainColPad = 0;   // gain column: right-aligned ON the plot's edge
    static constexpr int kSpecColX = 4,  kSpecColW = 28;    // level column: offsets from the plot's edge
                                                            // (4 + 28 = the whole gutter — the level
                                                            //  numbers end ON the component's edge)
    // The Range pill leans PAST the plot's edge into the gutter — it belongs to the gain scale it
    // heads, so it sits a touch further right than the numbers under it, and still clears the level
    // column's topmost number ("0", right-aligned at the far end of its box).
    static constexpr int kRangePillOverhang = 6;
    static constexpr int kLaneH       = 84;           // Fixed-lane reserve = strip + freq axis (kToolbarH + kBottomAxisH + gap)

    // Toolbar placement strategy (see setToolbarPlacement). Classic = the original centered-above behaviour.
    enum class ToolbarPlace { Classic, AnchorSide, Collision, Hybrid, FixedLane };
    ToolbarPlace toolbarPlace = ToolbarPlace::Classic;
    int  lastPlaceSlot = -1;                          // hysteresis: last chosen candidate slot (collision/hybrid)

    // Node<->strip leader line (see setLeaderMode) — uniform across every placement mode. Flash shows it
    // for ~1 s (with a fade tail) whenever the selection changes or the strip re-anchors/jumps.
    enum class Leader { Off, Flash, Always };
    Leader leaderOpt = Leader::Flash;
    juce::uint32 leaderFlashMs = 0;                   // Flash: timestamp of the last selection change / strip jump
    int  lastLeaderBand = -2, lastLeaderLane = -1;    // selection the leader endpoints were last computed for
    juce::Point<int> lastToolbarPos { -10000, -10000 };   // jump detection (an anchor hop re-arms the flash)
    juce::Point<float> leaderNode, leaderBar;         // connector endpoints (canvas coords)

    // Vertical dB scale (top-right combo, persisted). gainRange is the VISIBLE ± dB window; it auto-zooms out
    // while dragging a node past the edge (kGainSteps), so a node never gets stranded under the floating strip.
    double gainRange = 12.0;                          // visible ± dB (curve y-axis); NOT the gain clamp (that's kGainMax)
    juce::uint32 lastZoomMs = 0;                      // rate-limit for drag auto-zoom (one step per interval)
    FlatScaleButton gainScaleBtn;                     // vertical-scale picker (±3/6/12/30 dB) — flat, top-right

    bool  audLockGain = true;                         // Alt-drag sweeps frequency only (gain frozen)
    float lastDragFreq = 1000.0f;                     // last audition centre (for crisp modifier toggling)
    bool  placeGainFromDrag = false;                  // press-drag add: take gain from drag Y (not default)

    static constexpr double kFreqMin   = 20.0, kFreqMax = 28000.0;   // AXIS range: top runs ~½-oct past 20k so the >20k rolloff is visible
    // How much clear room a printed number wants before the scale drops to a sparser rung — the two
    // dials of every scale on this plot, tuned BY EYE and kept round on purpose. 50 px along the
    // bottom (numbers sit side by side there, and "20k" is wide); 25 px down a column, where a row
    // needs only its own height plus air.
    static constexpr double kLabelGapPx = 50.0;   // frequency captions, horizontally
    static constexpr double kRowGapPx   = 25.0;   // dB captions (gain + analyzer), vertically
    static constexpr double kFreqPlaceMax = 20000.0;                 // bands can only be PLACED up to here (= the freq param max); 20k–28k is display-only
    static constexpr double kGainMax   = 24.0;          // ± dB — the real gain-param clamp (drag / keyboard)
    static constexpr double kGainSteps[4] = { 3.0, 6.0, 12.0, 30.0 };   // selectable vertical-scale steps (visible ± dB)
    static constexpr double kSpecTop   = 6.0,  kSpecBottom = -90.0;
    static constexpr float  kNodeR     = eqview::handles::kNodeR;   // single home = HandleMath (drawing + hit-tests share it)
    static constexpr float  kAddThreshold = 36.0f;       // px from the curve to surface the "+" add button
    static constexpr float  kDynGrip   = 16.0f;          // resting offset of the dynamic handle below a node (px)
    static constexpr float  kDynBandW  = 7.0f;           // width of the range band drawn under the node
    static constexpr float  kPeakFallDb = 0.8f;          // peak-hold fall (~24 dB/s at 30 Hz)
    static constexpr double kTiltDbPerOct = 4.5;         // analyzer tilt — pink-noise comp, like Pro-Q / Neutron
    static constexpr double kTiltPivotHz  = 1000.0;      // tilt pivot (highs lift, lows drop around 1 kHz)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqCurveDisplay)
};
