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
        case 6:  return teq::FilterType::Notch;
        case 7:  return teq::FilterType::AllPass;
        case 8:  return teq::FilterType::Tilt;
        default: return teq::FilterType::Bell;
    }
}

static void addBand (juce::AudioProcessorValueTreeState::ParameterLayout& layout, int b)
{
    using namespace juce;
    const String n = "B" + String (b + 1) + " ";   // display prefix, e.g. "B1 Freq"

    layout.add (std::make_unique<AudioParameterBool>   (ParameterID { bandId (b, "on"),   1 }, n + "On", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { bandId (b, "type"), 1 }, n + "Type",
                                                        StringArray { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Band Pass", "Notch", "All Pass", "Tilt" }, 0));

    NormalisableRange<float> freqRange (20.0f, 20000.0f); freqRange.setSkewForCentre (1000.0f);   // matches the canvas range
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { bandId (b, "freq"), 1 }, n + "Freq",
                                                        freqRange, 1000.0f, AudioParameterFloatAttributes().withLabel ("Hz")));

    NormalisableRange<float> qRange (0.05f, 40.0f); qRange.setSkewForCentre (1.0f);
    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { bandId (b, "q"), 1 }, n + "Q", qRange, 1.0f));

    layout.add (std::make_unique<AudioParameterFloat>  (ParameterID { bandId (b, "gain"), 1 }, n + "Gain",
                                                        NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f, AudioParameterFloatAttributes().withLabel ("dB")));   // matches the canvas ±24

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { bandId (b, "slope"), 1 }, n + "Slope",
                                                        StringArray { "6 dB/oct", "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct" }, 1));
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
