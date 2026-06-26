// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "Parameters.h"

namespace tabby
{

juce::String bandId (int b, juce::StringRef suffix) { return "band" + juce::String (b) + "_" + suffix; }

teq::FilterType filterTypeFromChoice (int idx) noexcept
{
    switch (idx)
    {
        case 1:  return teq::FilterType::LowShelf;
        case 2:  return teq::FilterType::HighShelf;
        case 3:  return teq::FilterType::HighPass;
        case 4:  return teq::FilterType::LowPass;
        case 5:  return teq::FilterType::BandPass;
        default: return teq::FilterType::Bell;
    }
}

static void addBand (juce::AudioProcessorValueTreeState::ParameterLayout& layout, int b)
{
    using namespace juce;
    const String n = "B" + String (b + 1) + " ";   // display prefix, e.g. "B1 Freq"

    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { bandId (b, "on"),   1 }, n + "On", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { bandId (b, "type"), 1 }, n + "Type",
                                                        StringArray { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Band Pass" }, 0));

    NormalisableRange<float> freqRange (10.0f, 24000.0f); freqRange.setSkewForCentre (1000.0f);
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { bandId (b, "freq"), 1 }, n + "Freq",
                                                        freqRange, 1000.0f, AudioParameterFloatAttributes().withLabel ("Hz")));

    NormalisableRange<float> qRange (0.05f, 40.0f); qRange.setSkewForCentre (1.0f);
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { bandId (b, "q"), 1 }, n + "Q", qRange, 1.0f));

    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { bandId (b, "gain"), 1 }, n + "Gain",
                                                        NormalisableRange<float> (-30.0f, 30.0f, 0.01f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { bandId (b, "slope"), 1 }, n + "Slope",
                                                        StringArray { "12 dB/oct", "24 dB/oct" }, 0));
    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { bandId (b, "swept"), 1 }, n + "Swept", false));
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (int b = 0; b < kNumBands; ++b) addBand (layout, b);

    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "output", 1 }, "Output",
                                                             juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
                                                             juce::AudioParameterFloatAttributes().withLabel ("dB")));
    return layout;
}

} // namespace tabby
