// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "PluginProcessor.h"
#include "PluginEditor.h"

TabbyEqAudioProcessor::TabbyEqAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", tabby::createParameterLayout())
{
    for (int b = 0; b < tabby::kNumBands; ++b)
    {
        auto& p = bands[(size_t) b];
        p.on    = apvts.getRawParameterValue (tabby::bandId (b, "on"));
        p.type  = apvts.getRawParameterValue (tabby::bandId (b, "type"));
        p.freq  = apvts.getRawParameterValue (tabby::bandId (b, "freq"));
        p.q     = apvts.getRawParameterValue (tabby::bandId (b, "q"));
        p.gain  = apvts.getRawParameterValue (tabby::bandId (b, "gain"));
        p.slope = apvts.getRawParameterValue (tabby::bandId (b, "slope"));
        p.swept = apvts.getRawParameterValue (tabby::bandId (b, "swept"));
        p.bypass = apvts.getRawParameterValue (tabby::bandId (b, "bypass"));
    }
    outputGain = apvts.getRawParameterValue ("output");
}

void TabbyEqAudioProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock, getTotalNumOutputChannels());   // engine clamps to teq::kMaxChannels
    outputGainSmoothed.reset (sampleRate, 0.02);
    outputGainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputGain->load()));   // start at the saved trim — no ramp on load
    soloFilter.prepare (sampleRate, getTotalNumOutputChannels());
}

// Generic any-channel support up to the engine's cap: any MATCHED layout (mono, stereo, 5.1, 7.1,
// 7.1.4 Atmos, ambisonics, … up to teq::kMaxChannels), plus the one conventional convenience up-mix
// mono->stereo. We deliberately don't fan a mono source out onto surround/ambisonic buses — copying
// mono into B-format components or an LFE is a panner/encoder's job, not an EQ's. No down-mix either.
bool TabbyEqAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (out.isDisabled() || in.isDisabled()) return false;
    if (out.size() < 1 || out.size() > teq::EqEngine::kMaxChannels) return false;
    if (in == out) return true;                                                                  // matched: mono..16ch
    return in == juce::AudioChannelSet::mono() && out == juce::AudioChannelSet::stereo();         // mono->stereo only
}

void TabbyEqAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;   // FTZ/DAZ on the audio thread (engine also flushes per block)

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int n      = buffer.getNumSamples();
    const int nc     = juce::jmin (numOut, teq::EqEngine::kMaxChannels);   // channels we EQ
    if (nc <= 0) return;

    // Up-mix a mono input into stereo (the only non-matched layout we accept) => identical L/R.
    for (int c = juce::jmax (1, numIn); c < nc; ++c) buffer.copyFrom (c, 0, buffer, 0, 0, n);

    // Solo (band-listen): replace the output with a band-pass of the input at the soloed band's
    // freq/Q, so you hear only that region. Skips the normal EQ.
    const int solo = soloBand.load (std::memory_order_relaxed);
    if (solo >= 0 && solo < tabby::kNumBands)
    {
        const auto bp = readBand (solo);
        soloFilter.setParams (teq::FilterType::BandPass, bp.freq, juce::jlimit (0.5, 12.0, bp.Q), 0.0);
        for (int c = 0; c < nc; ++c)
        {
            float* d = buffer.getWritePointer (c);
            for (int s = 0; s < n; ++s) d[s] = soloFilter.processSample (c, d[s]);
        }
        soloFilter.flushDenormals();
        outputGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outputGain->load()));
        outputGainSmoothed.applyGain (buffer, n);
        return;
    }

    // Feed each band's params to the engine HERE (audio thread) so setBand/process share a thread
    // — the engine's contract. Smoothing + recompute-skip live inside the engine.
    for (int b = 0; b < tabby::kNumBands; ++b)
        engine.setBand (b, readBand (b));

    engine.process (buffer.getArrayOfWritePointers(), nc, n);

    outputGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outputGain->load()));
    outputGainSmoothed.applyGain (buffer, n);   // de-zippered output trim
}

void TabbyEqAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("stateVersion", kStateVersion, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void TabbyEqAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
    // `stateVersion` is available here for future migrations; v1 needs none.
}

teq::BandParams TabbyEqAudioProcessor::readBand (int b) const noexcept
{
    const auto& p = bands[(size_t) b];
    teq::BandParams bp;
    bp.on     = p.on->load()    > 0.5f;
    bp.type   = tabby::filterTypeFromChoice ((int) p.type->load());
    bp.freq   = (double) p.freq->load();
    bp.Q      = (double) p.q->load();
    bp.gainDb = (double) p.gain->load();
    static constexpr int kSlopeDb[] = { 6, 12, 24, 36, 48, 72, 96 };
    bp.slope  = kSlopeDb[juce::jlimit (0, 6, (int) p.slope->load())];
    bp.swept  = p.swept->load() > 0.5f;
    bp.bypass = p.bypass->load() > 0.5f;
    return bp;
}

juce::AudioProcessorEditor* TabbyEqAudioProcessor::createEditor()
{
    return new TabbyEqEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TabbyEqAudioProcessor();
}
