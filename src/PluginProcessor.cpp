// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "PluginProcessor.h"

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
    }
    outputGain = apvts.getRawParameterValue ("output");
}

void TabbyEqAudioProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    engine.prepare (sampleRate, maximumExpectedSamplesPerBlock, juce::jmin (getTotalNumOutputChannels(), 2));
}

bool TabbyEqAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;   // in == out
}

void TabbyEqAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;   // FTZ/DAZ on the audio thread (engine also flushes per block)

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int n      = buffer.getNumSamples();

    for (int i = numIn; i < numOut; ++i) buffer.clear (i, 0, n);

    // Feed each band's params to the engine HERE (audio thread) so setBand/process share a thread
    // — the engine's contract. Smoothing + recompute-skip live inside the engine.
    for (int b = 0; b < tabby::kNumBands; ++b)
    {
        const auto& p = bands[(size_t) b];
        teq::BandParams bp;
        bp.on     = p.on->load()    > 0.5f;
        bp.type   = tabby::filterTypeFromChoice ((int) p.type->load());
        bp.freq   = (double) p.freq->load();
        bp.Q      = (double) p.q->load();
        bp.gainDb = (double) p.gain->load();
        bp.slope  = ((int) p.slope->load()) == 1 ? 24 : 12;
        bp.swept  = p.swept->load() > 0.5f;
        engine.setBand (b, bp);
    }

    engine.process (buffer.getArrayOfWritePointers(), juce::jmin (numIn, numOut, 2), n);

    buffer.applyGain (juce::Decibels::decibelsToGain (outputGain->load()));
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

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TabbyEqAudioProcessor();
}
