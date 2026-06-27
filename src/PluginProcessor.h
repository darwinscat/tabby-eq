// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <teq/EqEngine.h>

#include "Parameters.h"

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
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    //==========================================================================
    // Analyzer + curve support for the editor (message thread).
    // Ref-counted so concurrent editors don't disable each other's analyzer.
    void setAnalyzerActive (bool shouldRun) noexcept
    {
        const int delta = shouldRun ? 1 : -1;
        engine.setSpectrumActive ((analyzerRefs.fetch_add (delta, std::memory_order_relaxed) + delta) > 0);
    }
    bool pullSpectrum (bool pre, float* dst) noexcept { return (pre ? engine.inputTap() : engine.outputTap()).tryPull (dst); }
    teq::BandParams readBand (int b) const noexcept;   // BandParams from the APVTS atomics

    void setSoloBand (int b) noexcept { soloBand.store (b, std::memory_order_relaxed); }   // -1 = no solo
    int  getSoloBand() const noexcept { return soloBand.load (std::memory_order_relaxed); }

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
    teq::EqEngine engine;

    struct BandPtrs
    {
        std::atomic<float>* on{}, *type{}, *freq{}, *q{}, *gain{}, *slope{}, *swept{}, *bypass{}, *route{};
    };
    std::array<BandPtrs, tabby::kNumBands> bands;
    std::atomic<float>* outputGain = nullptr;
    juce::LinearSmoothedValue<float> outputGainSmoothed { 1.0f };   // de-zippered output trim
    std::atomic<int> analyzerRefs { 0 };                            // editors needing the analyzer
    teq::Svf         soloFilter;                                    // band-listen band-pass (solo)
    std::atomic<int> soloBand { -1 };                               // soloed band index, or -1

    std::atomic<float> inPeak { 0.0f }, outPeak { 0.0f };   // max |sample| since last UI read (linear)
    std::atomic<bool>  inClip { false }, outClip { false }; // sticky >= 0 dBFS clip until the UI resets

    static constexpr int kStateVersion = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabbyEqAudioProcessor)
};
