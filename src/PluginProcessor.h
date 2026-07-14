// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <teq/EqEngine.h>
#include <teq/Smoother.h>
#include <felitronics/analysis/RollingSpectrumTap.h>   // analyzer taps (decoupled hop; selectable FFT size)
#include <felitronics/appkit/CompareHistory.h>

#include "Parameters.h"
#include "LinearPhase.h"
#include "NaturalPhase.h"
#include "AppPreferences.h"
#include "UpdateChecker.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

//==============================================================================
// One captured link-mirroring edit: which lane param moved (band / lane / field) AND the normalized value
// the listener saw. Capturing the value at enqueue time is load-bearing: the drain replays CAPTURED values
// in delivery order — re-reading the source param at drain time would let two linked lanes edited between
// drains corrupt each other (the first event's mirror overwrites the second lane's newer edit, which the
// second event would then re-read).
struct LinkEvent { int band = 0, lane = 0, field = 0; float value = 0.0f; };

// Bounded lock-free FIFO for the link-mirroring events. Producers are ANY thread (parameterValueChanged
// can fire on the audio thread under host automation); the single consumer is the processor's 30 Hz
// message-thread timer. This is the classic Vyukov bounded MPSC ring — fixed array, no alloc, no lock, no
// blocking. On a full ring push() drops the event and latches `overflowed` (the drain then discards the
// partial stream and resyncs — see PluginProcessor.cpp). Honesty note: the push itself is alloc/lock-free,
// but JUCE's listener DISPATCH that calls us briefly holds the parameter's listener lock — uncontended
// except during editor open/close listener churn, and inherent to every JUCE parameter listener or
// attachment, not added by this design.
class LinkFifo
{
public:
    LinkFifo() { for (unsigned i = 0; i < kCap; ++i) slots_[i].seq.store (i, std::memory_order_relaxed); }

    // Any thread. Returns false (and latches overflowed) when full.
    bool push (const LinkEvent& ev) noexcept
    {
        unsigned pos = head_.load (std::memory_order_relaxed);
        for (;;)
        {
            Slot& s = slots_[pos & kMask];
            const unsigned seq = s.seq.load (std::memory_order_acquire);
            const int dif = (int) (seq - pos);
            if (dif == 0)
            {
                if (head_.compare_exchange_weak (pos, pos + 1, std::memory_order_relaxed))
                {
                    s.ev = ev;
                    s.seq.store (pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (dif < 0) { overflowed.store (true, std::memory_order_relaxed); return false; }
            else               pos = head_.load (std::memory_order_relaxed);
        }
    }

    // Single consumer (the timer). Returns false when empty.
    bool pop (LinkEvent& out) noexcept
    {
        const unsigned pos = tail_.load (std::memory_order_relaxed);
        Slot& s = slots_[pos & kMask];
        const unsigned seq = s.seq.load (std::memory_order_acquire);
        const int dif = (int) (seq - (pos + 1));
        if (dif == 0)
        {
            out = s.ev;
            tail_.store (pos + 1, std::memory_order_relaxed);
            s.seq.store (pos + kCap, std::memory_order_release);
            return true;
        }
        return false;
    }

    std::atomic<bool> overflowed { false };

private:
    static constexpr unsigned kCap = 256, kMask = kCap - 1;   // power of two
    struct Slot { std::atomic<unsigned> seq { 0 }; LinkEvent ev; };
    Slot slots_[kCap];
    std::atomic<unsigned> head_ { 0 }, tail_ { 0 };
};

//==============================================================================
// TabbyEQ — the AudioProcessor. A thin adapter: it owns a teq::EqEngine, packs each band's APVTS atomics
// (five placement lanes) into teq::BandParams v2 and feeds the engine at the top of processBlock (so
// setBand/process run on the same thread, satisfying the engine's contract). Per-point Link FQ / Link Q are
// mirrored processor-side (host-safe: alloc/lock-free listener body → bounded FIFO → 30 Hz drain; see the
// LinkFifo honesty note about JUCE's own listener-dispatch lock). Versioned state with a v2→v3 migration.
//
// 🔴 Real-time rule: processBlock and callees never allocate, lock, do IO, or throw.
class TabbyEqAudioProcessor final : public juce::AudioProcessor,
                                    private juce::AudioProcessorParameter::Listener
{
public:
    TabbyEqAudioProcessor();
    ~TabbyEqAudioProcessor() override;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override { prepared.store (false, std::memory_order_relaxed); lp.releaseResources(); np.releaseResources(); }
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==========================================================================
    // Analyzer + curve support for the editor (message thread).
    // Ref-counted so concurrent editors don't disable each other's analyzer.
    void setAnalyzerActive (bool shouldRun) noexcept
    {
        analyzerRefs.fetch_add (shouldRun ? 1 : -1, std::memory_order_relaxed);   // taps are fed from processBlock (domain-aware)
    }
    // Pull the latest analyzer frame; reports the FFT order it was captured at (the UI discards a frame
    // whose order != the resolution it currently wants, so a live switch never shows a wrong-size frame).
    bool pullSpectrum (bool pre, float* dst, int& outOrder) noexcept { return (pre ? *preTap : *postTap).tryPull (dst, outOrder); }

    // Analyzer domain: which signal the spectrum shows — 0 Stereo (ch0) / 1 Mid (L+R)/2 / 2 Side (L-R)/2.
    void  setSpectrumDomain (int d) noexcept { spectrumDomain.store (d, std::memory_order_relaxed); }
    int   getSpectrumDomain() const noexcept { return spectrumDomain.load (std::memory_order_relaxed); }

    // Analyzer resolution: spectrum FFT order 10..14 (1024/2048/4096/8192/16384). Read once per block by
    // the audio thread; changing it force-publishes a fresh frame at the new size (click-free live switch).
    // The [10,14] clamp MUST stay within eqview::SpectrumPane's [kMinOrder,kMaxOrder]=[10,14] (kMaxOrder
    // rides RollingSpectrumTap's MaxOrder): the pane trusts the frame order to size its FFT, so a lower
    // order here would make it window a short frame's stale tail. Widen only together with the tap's
    // MaxOrder, SpectrumPane::kMinOrder, and the AnalyzerPanel menu.
    void  setSpectrumResolution (int order) noexcept { analyzerOrder.store (juce::jlimit (10, 14, order), std::memory_order_relaxed); }
    int   getSpectrumResolution() const noexcept     { return analyzerOrder.load (std::memory_order_relaxed); }
    float getCorrelation()   const noexcept { return correlation.load (std::memory_order_relaxed); }   // L/R phase correlation -1..+1
    teq::BandParams readBand (int b) const noexcept;   // BandParams v2 from the APVTS atomics (5 lanes)

    void setSoloBand (int b) noexcept { soloBand.store (b, std::memory_order_relaxed); }   // -1 = no solo
    int  getSoloBand() const noexcept { return soloBand.load (std::memory_order_relaxed); }

    // The UI's active lane for a band (0..4 = teq::Lane order). Drives the solo band-pass frequency and is
    // persisted as the per-band `activeLane` ValueTree property. Message thread.
    void setBandActiveLane (int band, int lane) noexcept;
    int  getBandActiveLane (int band) const noexcept   // the processor-side mirror (what solo actually uses)
    {
        return band >= 0 && band < tabby::kNumBands ? activeLaneAtom[(size_t) band].load (std::memory_order_relaxed) : -1;
    }

    // Drag-audition: listen to a narrow band-pass at an arbitrary frequency, independent of the band
    // list (so it works even while placing a not-yet-created band). RT-safe (atomics only).
    void setAudition (bool on, float freqHz = 1000.0f, float q = 6.0f) noexcept
    {
        auditionFreq.store (freqHz, std::memory_order_relaxed);
        auditionQ.store    (q,      std::memory_order_relaxed);
        auditionOn.store   (on,     std::memory_order_relaxed);
    }

    // IN/OUT level meters for the editor. The audio thread accumulates the peak |sample| since the
    // last UI read (read-and-reset); clip is sticky until the UI clears it. All lock-free.
    float readInPeak()  noexcept { return inPeak.exchange  (0.0f, std::memory_order_relaxed); }
    float readOutPeak() noexcept { return outPeak.exchange (0.0f, std::memory_order_relaxed); }
    bool  inClipped()   const noexcept { return inClip.load  (std::memory_order_relaxed); }
    bool  outClipped()  const noexcept { return outClip.load (std::memory_order_relaxed); }
    void  clearInClip()  noexcept { inClip.store  (false, std::memory_order_relaxed); }
    void  clearOutClip() noexcept { outClip.store (false, std::memory_order_relaxed); }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // Versioned state from day one (so later versions don't break DAW sessions). v3 migrates v2 by value.
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Opt-in update check. Owned here (not the editor) so it survives the window closing, is one per
    // plugin instance, and shares the single global PropertiesFile; the (i) button drives + reads it.
    tabby::UpdateChecker& updateChecker() { return updateCheckerInstance; }

    //==========================================================================
    // A/B/C/D compare + undo/redo — the shared felitronics-appkit CompareHistory engine in
    // PerRegister mode: each register carries its OWN undo/redo history, and a register SWITCH is
    // NOT an undo step (its inverse is re-selecting the slot). The opaque snapshot is the whole
    // APVTS state tree (params + view/session properties such as activeLane). Message thread only.
    static constexpr int kNumSnapshots = 4;
    void switchToSnapshot (int index) { history.switchTo (index); }
    int  getActiveSnapshot() const noexcept { return history.active(); }

    // "Modified since you dialed it in" marker for the A/B/C/D buttons: has this register
    // accumulated edits (its own undo history is non-empty)?
    bool snapshotEdited (int i) const noexcept
    {
        return juce::isPositiveAndBelow (i, kNumSnapshots) && history.registerEdited (i);
    }

    // A register "has content" (a valid copy SOURCE) if it is the active/live one or holds a stored
    // snapshot. Drives the copy menu (only content-bearing registers are offered as sources).
    bool snapshotHasContent (int i) const noexcept
    {
        return i == history.active() || (juce::isPositiveAndBelow (i, kNumSnapshots) && history.registerTree (i).has_value());
    }

    // Copy the whole state of one A/B/C/D register into another — one discrete undoable edit in the
    // TARGET register's history (Recall D->B edits B; Stamp B->D edits D).
    void copySnapshot (int from, int to)
    {
        if (juce::isPositiveAndBelow (from, kNumSnapshots) && juce::isPositiveAndBelow (to, kNumSnapshots))
            history.copyRegister (from, to);
    }

    // Register i's state tree (active -> a fresh live capture), for the clipboard. Deep copy —
    // never a handle aliasing an engine-stored snapshot.
    juce::ValueTree snapshotState (int i)
    {
        if (! juce::isPositiveAndBelow (i, kNumSnapshots)) return {};
        return i == history.active() ? apvts.copyState()
                                     : history.registerTree (i).value_or (juce::ValueTree()).createCopy();
    }

    // Clipboard paste: only a TabbyEQ state tree (the APVTS root type) is pasteable.
    bool canPasteState (const juce::ValueTree& t) const { return t.hasType (apvts.state.getType()); }
    void pasteState (int toReg, const juce::ValueTree& t)
    {
        if (juce::isPositiveAndBelow (toReg, kNumSnapshots) && canPasteState (t))
            history.applyEdit (toReg, t, "Paste");
    }

    // Undo / redo. The editor pumps undoTick() from its 10 Hz timer (settleTicks is a tick COUNT,
    // not wall-clock, so one steady pump). Undo/redo act on the active register only.
    void undoTick() { history.tick(); }
    bool undo()     { return history.undo(); }
    bool redo()     { return history.redo(); }
    bool canUndo() const noexcept { return history.canUndo(); }
    bool canRedo() const noexcept { return history.canRedo(); }
    int  undoDepth() const noexcept { return history.undoDepth(); }
    int  redoDepth() const noexcept { return history.redoDepth(); }
    juce::String peekUndoLabel() const { return history.peekUndoLabel(); }
    juce::String peekRedoLabel() const { return history.peekRedoLabel(); }

    // Gesture brackets: everything between begin/end is ONE labelled undo step — the tool for a
    // node drag (grab = begin, release = end). Nestable; see CompareHistory's gesture contract.
    // Both drain the link-mirror FIFO first, so pending mirror writes land on the correct side of
    // the step boundary: pre-gesture mirrors fold into the flushed pre-burst, and the drag's last
    // mirrors fold into the gesture step instead of leaking out as a separate settle burst ~33 ms
    // after mouse-up (the drain timer would otherwise race endGesture).
    void beginHistoryGesture (const juce::String& label) { ++openHistoryGestures; drainLinkFifo(); history.beginGesture (label); }
    void endHistoryGesture()                             { drainLinkFifo(); history.endGesture(); openHistoryGestures = juce::jmax (0, openHistoryGestures - 1); }
    // Any history gesture (node drag, slider drag, lane-menu action) is open — the editor gates
    // history NAVIGATION on this (a switch/undo/paste mid-gesture is the engine's misuse path).
    bool historyGestureOpen() const noexcept             { return openHistoryGestures > 0; }

    // RAII bracket for UI actions with early returns — goes through begin/endHistoryGesture (NOT
    // the engine's raw ScopedGesture), so the FIFO drains and the open-gesture gate stay correct.
    struct ScopedHistoryGesture
    {
        TabbyEqAudioProcessor& p;
        ScopedHistoryGesture (TabbyEqAudioProcessor& proc, const juce::String& label) : p (proc) { p.beginHistoryGesture (label); }
        ~ScopedHistoryGesture() { p.endHistoryGesture(); }
        JUCE_DECLARE_NON_COPYABLE (ScopedHistoryGesture)
    };

    // Reset every parameter to its default as ONE non-undoable programmatic bulk write (the phase
    // decision: Reset records no step; earlier history still walks back past it). See the .cpp for
    // why the link-FIFO is drained inside the scope.
    void resetAllToDefaults();

    //==========================================================================
    // Preset files — the family convention (…/Darwin's Cat/TabbyEQ/Presets, *.tabbyeq). A preset is
    // the FLAT live state tree (v3 lane format) as XML text; loading routes through
    // setStateInformation's flat-tree path: live sound replaced, B/C/D reset, fresh clean history.
    static juce::File presetDirectory();
    bool saveStateFile (const juce::File& f);   // live state -> XML text file (portable preset)
    bool loadStateFile (const juce::File& f);   // XML file -> validated load (false = not a TabbyEQ state)

    // Programmatic bulk-write gate (preset apply / reset / search->treat): writes apply but record
    // no history. RAII: felitronics::appkit::CompareHistory::ScopedSuppress ss (proc.compareHistory());
    felitronics::appkit::CompareHistory& compareHistory() noexcept { return history; }

    // Event-driven history revision: every history mutation (commit / undo / redo / switch / copy /
    // clear / load) bumps it — the editor polls this instead of onHistoryChanged callbacks (cheap,
    // and safe across editor open/close).
    unsigned historyRevision() const noexcept { return historyRev.load (std::memory_order_relaxed); }

    // Bumped on every APPLY (undo / redo / switch / copy / load) — i.e. whenever the live state was
    // replaced wholesale and the editor must re-sync its state-tree mirrors (view props, lane caches).
    unsigned applyRevision() const noexcept { return applyRev.load (std::memory_order_relaxed); }

private:
    // AudioProcessorParameter::Listener — may fire on ANY thread. The body only pushes a captured link
    // event + wakes the drain (alloc/lock-free; JUCE's dispatch itself holds the listener lock — see LinkFifo).
    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int, bool) override {}

    void lpTick();          // message-thread: feed the linear-phase builder + track mode/quality changes
    void onTimer();         // message-thread 30 Hz: drain link mirrors, then lpTick
    void drainLinkFifo();   // message-thread: replay captured mirror events in delivery order (or resync on overflow)
    void mirrorEvent (const LinkEvent& ev);                        // replay ONE captured edit onto the point's enabled lanes
    void applyToEnabledLanes (int band, const char* field, float v01);   // tagged writes; disabled lanes untouched
    void resyncAllLinks();  // idempotent snap of every linked point from its active lane (overflow recovery)
    int  resyncActiveLane (int band) const;                 // active lane from state prop / lowest-enabled fallback

    // The CompareHistory applyLive seam AND the shared load routine: replace the APVTS state from a
    // snapshot, then re-sync everything derived from it (link mirrors, per-band active lanes).
    void applyLiveState (const juce::ValueTree& t);

    // appPreferencesInstance MUST precede updateCheckerInstance: the checker takes it by reference,
    // so it has to be constructed first (members init in declaration order). SharedResourcePointer =
    // ONE AppPreferences per host process, shared by every TabbyEQ instance (see AppPreferences.h).
    juce::SharedResourcePointer<tabby::AppPreferences> appPreferencesInstance;   // process-wide PropertiesFile owner
    tabby::UpdateChecker updateCheckerInstance;                                  // version + opt-in update check (built in the ctor)

    teq::EqEngine engine;

    // Analyzer rolling taps (pre/post) — owned here, NOT borrowed from the engine: the resolution + the
    // Mid/Side domain are plugin concerns, and the FIR paths bypass engine.process() entirely. The order-14
    // ring (16384) serves any selected FFT size 1024..16384 from one buffer; fed + published in processBlock.
    // Heap-allocated (each is ~128 KB): kept off the AudioProcessor's inline footprint so stack-allocating
    // the processor (unit tests do, ×25) stays cheap. Allocated once at construction — never in processBlock.
    // (const unique_ptr → `preTap->reset()` is the tap's reset; a stray `preTap.reset()` won't compile.)
    const std::unique_ptr<felitronics::analysis::RollingSpectrumTap> preTap  { std::make_unique<felitronics::analysis::RollingSpectrumTap>() };
    const std::unique_ptr<felitronics::analysis::RollingSpectrumTap> postTap { std::make_unique<felitronics::analysis::RollingSpectrumTap>() };
    std::atomic<int> analyzerHopBase { 1600 };                    // ~30 fps hop target in samples (set from fs in prepareToPlay; atomic — read on the audio thread)

    LinearPhaseEq lp;                                             // Linear-phase convolution path (exact zero-phase)
    NaturalPhase  np;                                             // Natural-phase convolution path (mixed phase, blend k)
    static constexpr int kNaturalQuality = 1;                    // Natural's fixed FIR length (L = 4096); the quality combo is Linear-only
    std::atomic<float>* phaseMode   = nullptr;                    // 0 = Zero Latency (IIR) · 1 = Natural Phase (FIR) · 2 = Linear Phase (FIR)
    std::atomic<float>* lpQuality   = nullptr;                    // 0..3 -> Linear FIR length
    std::atomic<float>* phaseAmount = nullptr;                    // Natural blend k (0 linear … 1 minimum phase)
    // Master "fully prepared" flag: true only after prepareToPlay() finishes building the engine + BOTH FIR
    // paths; gates BOTH the audio thread and lpTick(). Atomic + release/acquire so a thread that sees `true`
    // also sees the fully-built engine/FIR state (publish pattern).
    std::atomic<bool> prepared { false };
    int  lastQuality = -1;
    int  lastMode    = 0;                                         // 0/1/2 — track mode changes (re-report latency)
    float lastK      = -1.0f;                                     // track k changes (re-prepare Natural)

    struct LpUpdater : juce::Timer { TabbyEqAudioProcessor& p; explicit LpUpdater (TabbyEqAudioProcessor& pp) : p (pp) {}
                                     void timerCallback() override { p.onTimer(); } } lpUpdater { *this };

    // Per-band atomic parameter pointers — the shared point fields plus five placement lanes.
    struct LanePtrs { std::atomic<float>* on{}, *freq{}, *q{}, *gain{}, *slope{}, *byp{}; };
    struct BandPtrs { std::atomic<float>* on{}, *type{}, *swept{}, *bypass{}; LanePtrs lane[teq::kNumLanes]; };
    std::array<BandPtrs, tabby::kNumBands> bands;

    std::atomic<float>* outputGain = nullptr;
    teq::LinearSmoother outputGainSmoothed { 1.0f };                // de-zippered output trim (core, JUCE-free)
    std::atomic<int> analyzerRefs { 0 };                            // editors needing the analyzer
    teq::Svf         soloFilter;                                    // band-listen band-pass (solo)
    std::atomic<int> soloBand { -1 };                               // soloed band index, or -1
    std::atomic<int> activeLaneAtom[tabby::kNumBands];              // UI active lane per band (-1 = unset → lowest-enabled)
    std::atomic<bool>  auditionOn   { false };                      // drag-audition active (narrow listen)
    std::atomic<float> auditionFreq { 1000.0f }, auditionQ { 6.0f };

    // Link mirroring: a bounded FIFO fed by parameterValueChanged (any thread), drained on the 30 Hz timer.
    // Events carry the exact FIELD + captured normalized VALUE; the drain does no type mapping and no
    // source re-reads (see LinkEvent).
    LinkFifo           linkFifo;
    std::atomic<bool>  linkDirty  { false };                        // "events pending" (spec's dirty flag)
    std::vector<int8_t> linkField;                                  // by parameterIndex: -1 none / 0 freq / 1 q / 2 slope
    std::vector<int16_t> linkBand, linkLaneIdx;                     // by parameterIndex: band + lane of that param
    static constexpr int kFieldFreq = 0, kFieldQ = 1, kFieldSlope = 2;
    static constexpr const char* kLinkFieldNames[3] = { "freq", "q", "slope" };

    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };   // max |sample| since last UI read (linear)
    std::atomic<bool>  inClip { false }, outClip { false }; // sticky >= 0 dBFS clip until the UI resets

    std::atomic<int>   spectrumDomain { 0 };   // analyzer domain: 0 Stereo (ch0) / 1 Mid / 2 Side
    std::atomic<int>   analyzerOrder  { 11 };  // analyzer FFT order 10..14 → 1024/2048/4096/8192/16384 (2048 default)
    std::atomic<float> correlation { 1.0f };   // L/R phase correlation (-1..+1) for the meter
    float corrState = 1.0f;                    // audio-thread smoothing state for the correlation meter

    // The shared appkit undo/redo + A/B/C/D engine (PerRegister mode). The opaque seam: captureLive
    // = apvts.copyState(), applyLive = applyLiveState(). Declared AFTER apvts (public above) — the
    // engine's construction-time capture-stability assert captures through the live APVTS.
    felitronics::appkit::CompareHistory history;
    std::atomic<unsigned> historyRev { 0 };    // bumped by onHistoryChanged; the editor polls historyRevision()
    std::atomic<unsigned> applyRev   { 0 };    // bumped by onAfterApply; the editor polls applyRevision()
    int openHistoryGestures = 0;               // refcount of open begin/endHistoryGesture brackets (message thread)

    static constexpr int kStateVersion = 4;   // v4: session root = CompareHistory <Workspace> envelope (live +
                                              // A/B/C/D + active); the inner state trees keep the v3 lane format

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabbyEqAudioProcessor)
};
