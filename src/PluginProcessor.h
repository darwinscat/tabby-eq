// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <teq/EqEngine.h>
#include <teq/Smoother.h>

#include "Parameters.h"
#include "LinearPhase.h"
#include "NaturalPhase.h"

#include <array>
#include <atomic>
#include <vector>

//==============================================================================
// Bounded lock-free FIFO for the link-mirroring events (band / lane / kind). Producers are ANY thread
// (parameterValueChanged can fire on the audio thread under host automation); the single consumer is the
// processor's 30 Hz message-thread timer. This is the classic Vyukov bounded MPSC ring — fixed array, no
// alloc, no lock, no blocking. On a full ring push() drops the event and latches `overflowed` (the drain
// then discards the partial stream and resyncs — see PluginProcessor.cpp).
class LinkFifo
{
public:
    LinkFifo() { for (unsigned i = 0; i < kCap; ++i) slots_[i].seq.store (i, std::memory_order_relaxed); }

    // Any thread. Returns false (and latches overflowed) when full.
    bool push (int band, int lane, int kind) noexcept
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
                    s.band = band; s.lane = lane; s.kind = kind;
                    s.seq.store (pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (dif < 0) { overflowed.store (true, std::memory_order_relaxed); return false; }
            else               pos = head_.load (std::memory_order_relaxed);
        }
    }

    // Single consumer (the timer). Returns false when empty.
    bool pop (int& band, int& lane, int& kind) noexcept
    {
        const unsigned pos = tail_.load (std::memory_order_relaxed);
        Slot& s = slots_[pos & kMask];
        const unsigned seq = s.seq.load (std::memory_order_acquire);
        const int dif = (int) (seq - (pos + 1));
        if (dif == 0)
        {
            band = s.band; lane = s.lane; kind = s.kind;
            tail_.store (pos + 1, std::memory_order_relaxed);
            s.seq.store (pos + kCap, std::memory_order_release);
            return true;
        }
        return false;
    }

    std::atomic<bool> overflowed { false };

private:
    static constexpr unsigned kCap = 256, kMask = kCap - 1;   // power of two
    struct Slot { std::atomic<unsigned> seq { 0 }; int band = 0, lane = 0, kind = 0; };
    Slot slots_[kCap];
    std::atomic<unsigned> head_ { 0 }, tail_ { 0 };
};

//==============================================================================
// TabbyEQ — the AudioProcessor. A thin adapter: it owns a teq::EqEngine, packs each band's APVTS atomics
// (five placement lanes) into teq::BandParams v2 and feeds the engine at the top of processBlock (so
// setBand/process run on the same thread, satisfying the engine's contract). Per-point Link FQ / Link Q are
// mirrored processor-side (host-safe: RT-safe listener → bounded FIFO → 30 Hz drain). Versioned state with a
// v2→v3 migration.
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
    bool pullSpectrum (bool pre, float* dst) noexcept { return (pre ? engine.inputTap() : engine.outputTap()).tryPull (dst); }

    // Analyzer domain: which signal the spectrum shows — 0 Stereo (ch0) / 1 Mid (L+R)/2 / 2 Side (L-R)/2.
    void  setSpectrumDomain (int d) noexcept { spectrumDomain.store (d, std::memory_order_relaxed); }
    int   getSpectrumDomain() const noexcept { return spectrumDomain.load (std::memory_order_relaxed); }
    float getCorrelation()   const noexcept { return correlation.load (std::memory_order_relaxed); }   // L/R phase correlation -1..+1
    teq::BandParams readBand (int b) const noexcept;   // BandParams v2 from the APVTS atomics (5 lanes)

    void setSoloBand (int b) noexcept { soloBand.store (b, std::memory_order_relaxed); }   // -1 = no solo
    int  getSoloBand() const noexcept { return soloBand.load (std::memory_order_relaxed); }

    // The UI's active lane for a band (0..4 = teq::Lane order). Drives the solo band-pass frequency and is
    // persisted as the per-band `activeLane` ValueTree property. Message thread.
    void setBandActiveLane (int band, int lane) noexcept;

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

private:
    // AudioProcessorParameter::Listener — RT-safe: pushes a link event + wakes the drain. May fire on ANY thread.
    void parameterValueChanged (int parameterIndex, float newValue) override;
    void parameterGestureChanged (int, bool) override {}

    void lpTick();          // message-thread: feed the linear-phase builder + track mode/quality changes
    void onTimer();         // message-thread 30 Hz: drain link mirrors, then lpTick
    void drainLinkFifo();   // message-thread: replay mirror events (or resync on overflow)
    void mirrorOne (int band, int lane, int kind);          // mirror one event to the point's other enabled lanes
    void mirrorField (int band, int srcLane, const char* field);   // copy srcLane's `field` to the enabled others
    void resyncAllLinks();  // idempotent snap of every linked point from its active lane (overflow recovery)
    int  resyncActiveLane (int band) const;                 // active lane from state prop / lowest-enabled fallback

    teq::EqEngine engine;

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
    LinkFifo           linkFifo;
    std::atomic<bool>  linkDirty  { false };                        // "events pending" (spec's dirty flag)
    std::atomic<bool>  mirrorGuard { false };                       // re-entrancy tag: our own mirror writes never re-enqueue
    std::vector<int8_t> linkKind;                                   // by parameterIndex: -1 none / 0 Freq / 1 Width
    std::vector<int16_t> linkBand, linkLaneIdx;                     // by parameterIndex: band + lane of that param
    static constexpr int kKindFreq = 0, kKindWidth = 1;

    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };   // max |sample| since last UI read (linear)
    std::atomic<bool>  inClip { false }, outClip { false }; // sticky >= 0 dBFS clip until the UI resets

    std::atomic<int>   spectrumDomain { 0 };   // analyzer domain: 0 Stereo (ch0) / 1 Mid / 2 Side
    std::atomic<float> correlation { 1.0f };   // L/R phase correlation (-1..+1) for the meter
    float corrState = 1.0f;                    // audio-thread smoothing state for the correlation meter

    static constexpr int kStateVersion = 3;   // v3: placement lanes (ST/L/R/M/S), shared type/swept/bypass

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabbyEqAudioProcessor)
};
