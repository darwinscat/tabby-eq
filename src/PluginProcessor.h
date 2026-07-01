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

//==============================================================================
// TabbyEQ — the AudioProcessor. A thin adapter: it owns a teq::EqEngine, packs each band's
// APVTS atomics into teq::BandParams and feeds the engine at the top of processBlock (so
// setBand/process run on the same thread, satisfying the engine's contract). Slice 2 ships the
// GenericAudioProcessorEditor (free knobs); the semantic editor is slice 3.
//
// 🔴 Real-time rule: processBlock and callees never allocate, lock, do IO, or throw.
class TabbyEqAudioProcessor final : public juce::AudioProcessor
{
public:
    TabbyEqAudioProcessor();
    ~TabbyEqAudioProcessor() override = default;

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
    teq::BandParams readBand (int b) const noexcept;   // BandParams from the APVTS atomics

    void setSoloBand (int b) noexcept { soloBand.store (b, std::memory_order_relaxed); }   // -1 = no solo
    int  getSoloBand() const noexcept { return soloBand.load (std::memory_order_relaxed); }

    // Drag-audition: listen to a narrow band-pass at an arbitrary frequency, independent of the band
    // list (so it works even while placing a not-yet-created band). The editor drives this while a
    // node/add is dragged with the audition modifier held. RT-safe (atomics only).
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

    // Versioned state from day one (so later versions don't break DAW sessions).
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    void lpTick();   // message-thread: feed the linear-phase builder + track mode/quality changes

    teq::EqEngine engine;

    LinearPhaseEq lp;                                             // Linear-phase convolution path (exact zero-phase)
    NaturalPhase  np;                                             // Natural-phase convolution path (mixed phase, blend k)
    static constexpr int kNaturalQuality = 1;                    // Natural's fixed FIR length (L = 4096); the quality combo is Linear-only
    std::atomic<float>* phaseMode   = nullptr;                    // 0 = Zero Latency (IIR) · 1 = Natural Phase (FIR) · 2 = Linear Phase (FIR)
    std::atomic<float>* lpQuality   = nullptr;                    // 0..3 -> Linear FIR length
    std::atomic<float>* phaseAmount = nullptr;                    // Natural blend k (0 linear … 1 minimum phase)
    // Master "fully prepared" flag: true only after prepareToPlay() finishes building the engine + BOTH
    // FIR paths; false before the first prepare and after releaseResources(). Gates BOTH the audio thread
    // (processBlock returns early when unprepared → the adapter OWNS its lifecycle safety instead of leaning
    // on the core's unprepared guards) and lpTick(). Atomic + release/acquire so the audio thread that sees
    // `true` also sees the fully-built engine/FIR state (publish pattern); this is the tabby-side mirror of
    // felitronics-core's own use-before-prepare hardening.
    std::atomic<bool> prepared { false };
    int  lastQuality = -1;
    int  lastMode    = 0;                                         // 0/1/2 — track mode changes (re-report latency)
    float lastK      = -1.0f;                                     // track k changes (re-prepare Natural)

    // Mode switching is a hard cut — seams on a deliberate, rare click are fine. (The cross-path
    // crossfade that used to live here is gone: its audio-thread state got corrupted by the
    // latency-change re-prepare, which made "Natural" fall silent. We also do NOT reset the convolver on
    // Linear resume — that stranded it on its empty bank, the "Linear silent until you nudge a band" bug;
    // the core's own Pending→crossfade primes the IR from prepare()/lpTick instead.)
    struct LpUpdater : juce::Timer { TabbyEqAudioProcessor& p; explicit LpUpdater (TabbyEqAudioProcessor& pp) : p (pp) {}
                                     void timerCallback() override { p.lpTick(); } } lpUpdater { *this };

    struct BandPtrs
    {
        std::atomic<float>* on{}, *type{}, *freq{}, *q{}, *gain{}, *slope{}, *swept{}, *bypass{};
        std::atomic<float>* ms{}, *sOn{}, *sType{}, *sFreq{}, *sQ{}, *sGain{}, *sSlope{}, *sBypass{};   // M/S Side lane
    };
    std::array<BandPtrs, tabby::kNumBands> bands;
    std::atomic<float>* outputGain = nullptr;
    teq::LinearSmoother outputGainSmoothed { 1.0f };                // de-zippered output trim (core, JUCE-free)
    std::atomic<int> analyzerRefs { 0 };                            // editors needing the analyzer
    teq::Svf         soloFilter;                                    // band-listen band-pass (solo)
    std::atomic<int> soloBand { -1 };                               // soloed band index, or -1
    std::atomic<bool>  auditionOn   { false };                      // drag-audition active (narrow listen)
    std::atomic<float> auditionFreq { 1000.0f }, auditionQ { 6.0f };

    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };   // max |sample| since last UI read (linear)
    std::atomic<bool>  inClip { false }, outClip { false }; // sticky >= 0 dBFS clip until the UI resets

    std::atomic<int>   spectrumDomain { 0 };   // analyzer domain: 0 Stereo (ch0) / 1 Mid / 2 Side
    std::atomic<float> correlation { 1.0f };   // L/R phase correlation (-1..+1) for the meter
    float corrState = 1.0f;                    // audio-thread smoothing state for the correlation meter

    static constexpr int kStateVersion = 2;   // v2: route removed, M/S Side lane added (additive)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabbyEqAudioProcessor)
};
