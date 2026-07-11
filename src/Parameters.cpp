// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "Parameters.h"

namespace tabby
{

juce::String bandId (int b, juce::StringRef suffix) { return "band" + juce::String (b) + "_" + suffix; }

namespace
{
    const char* kLaneKeys[5]  = { "st", "l", "r", "m", "s" };
    const char* kLaneWords[5] = { "Stereo", "Left", "Right", "Mid", "Side" };
}

const char* laneKey  (int lane) noexcept { return kLaneKeys [juce::jlimit (0, kNumLanes - 1, lane)]; }
const char* laneWord (int lane) noexcept { return kLaneWords[juce::jlimit (0, kNumLanes - 1, lane)]; }

juce::String laneParamId (int b, int lane, juce::StringRef field)
{
    return bandId (b, juce::String (laneKey (lane)) + "_" + field);
}

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

double defaultQFor (teq::FilterType t) noexcept
{
    return (t == teq::FilterType::HighPass || t == teq::FilterType::LowPass
         || t == teq::FilterType::LowShelf || t == teq::FilterType::HighShelf) ? 0.707 : 1.0;
}

void snapQOnTypeSwitch (juce::AudioProcessorValueTreeState& apvts, int band, int oldChoiceIdx, int newChoiceIdx)
{
    const double oldDef = defaultQFor (filterTypeFromChoice (oldChoiceIdx));
    const double newDef = defaultQFor (filterTypeFromChoice (newChoiceIdx));
    if (juce::approximatelyEqual (oldDef, newDef))
        return;

    // "Still at the old default" — 1e-4 covers the skewed Q range's set/get float roundtrip while a
    // deliberate near-default hand edit (0.71 typed against 0.707) stays out of "untouched".
    auto atOldDef = [&apvts, band, oldDef] (int lane)
    {
        auto* raw = apvts.getRawParameterValue (laneParamId (band, lane, "q"));
        return raw != nullptr && std::abs (raw->load() - (float) oldDef) < 1.0e-4f;
    };
    auto laneEnabled = [&apvts, band] (int lane)
    {
        auto* on = apvts.getRawParameterValue (laneParamId (band, lane, "on"));
        return on != nullptr && on->load() > 0.5f;
    };
    auto write = [&apvts, band] (int lane, double v)
    {
        if (auto* q = apvts.getParameter (laneParamId (band, lane, "q")))
        {
            q->beginChangeGesture();
            q->setValueNotifyingHost (q->convertTo0to1 ((float) v));
            q->endChangeGesture();
        }
    };

    // ENABLED lanes only: a split COPIES freq/q/slope from the source lane (see LaneMenu), so a
    // dormant lane's stored Q never survives into a split — snapping it would only spray the host
    // with parameter edits.
    if ((bool) apvts.state.getProperty (bandId (band, "linkQ"), false))
    {
        // Linked Q = ONE logical width. Snap only when EVERY enabled lane still sits at the old
        // default, and write a SINGLE lane — the processor's link mirror propagates it. Per-lane
        // writes here would enqueue snap events into the same mirror FIFO as a just-made hand edit
        // and replay the default OVER it at the 30 Hz drain; that hand edit's raw value is already
        // visible on its own lane, so this all-at-default gate sees it and backs off race-free.
        for (int L = 0; L < kNumLanes; ++L)
            if (laneEnabled (L) && ! atOldDef (L))
                return;
        for (int L = 0; L < kNumLanes; ++L)
            if (laneEnabled (L)) { write (L, newDef); return; }
        return;
    }

    for (int L = 0; L < kNumLanes; ++L)
        if (laneEnabled (L) && atOldDef (L))
            write (L, newDef);
}

static void addBand (juce::AudioProcessorValueTreeState::ParameterLayout& layout, int b)
{
    using namespace juce;
    const String n = "B" + String (b + 1) + " ";   // display prefix, e.g. "B1 Freq"
    const StringArray typeItems  { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Band Pass", "Notch", "All Pass", "Tilt" };
    const StringArray slopeItems { "6 dB/oct", "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct" };
    auto intHz = [] (float v, int) { return juce::String (juce::roundToInt (v)) + " Hz"; };
    auto one   = [] (float v, int) { return juce::String (v, 1); };
    auto oneDb = [] (float v, int) { return juce::String (v, 1) + " dB"; };

    // One group per band ("Band 4") holding all 34 of its params — the Pro-Q-style DAW tree. Ids are flat
    // (band{b}_… / band{b}_{k}_…), so getParameter()/getRawParameterValue() lookups are unchanged by grouping.
    auto grp = std::make_unique<AudioProcessorParameterGroup> ("band" + String (b), "Band " + String (b + 1), "|");

    // --- point-level (shared across lanes): on / type / swept / bypass ---
    grp->addChild (std::make_unique<AudioParameterBool>   (ParameterID { bandId (b, "on"),     1 }, n + "On",     false));
    grp->addChild (std::make_unique<AudioParameterChoice> (ParameterID { bandId (b, "type"),   1 }, n + "Type",   typeItems, 0));
    grp->addChild (std::make_unique<AudioParameterBool>   (ParameterID { bandId (b, "swept"),  1 }, n + "Swept",  false));
    grp->addChild (std::make_unique<AudioParameterBool>   (ParameterID { bandId (b, "bypass"), 1 }, n + "Bypass", false));

    // --- five placement lanes (ST / L / R / M / S), full param set each ---
    for (int L = 0; L < kNumLanes; ++L)
    {
        const bool   isSt = (L == 0);
        const String word = laneWord (L);
        // Collision-free display names: the ST lane DROPS the lane word for the point-unique fields
        // (Freq/Q/Gain/Slope) so unsplit automation reads like a normal EQ, but KEEPS it for On/Bypass
        // (the point already owns "B4 On" / "B4 Bypass"). Other lanes always carry the word.
        auto nm = [&] (const char* field, bool dropForSt) -> String
        {
            if (isSt && dropForSt) return n + field;              // "B4 Freq"
            return n + word + " " + field;                        // "B4 Left Freq" / "B4 Stereo On"
        };

        NormalisableRange<float> freqRange (20.0f, 20000.0f); freqRange.setSkewForCentre (1000.0f);
        NormalisableRange<float> qRange    (0.05f, 40.0f);    qRange.setSkewForCentre (1.0f);

        grp->addChild (std::make_unique<AudioParameterBool>   (ParameterID { laneParamId (b, L, "on"),   1 }, nm ("On", false), isSt));
        grp->addChild (std::make_unique<AudioParameterFloat>  (ParameterID { laneParamId (b, L, "freq"), 1 }, nm ("Freq", true),
                                                               freqRange, 1000.0f, AudioParameterFloatAttributes().withStringFromValueFunction (intHz)));
        grp->addChild (std::make_unique<AudioParameterFloat>  (ParameterID { laneParamId (b, L, "q"), 1 }, nm ("Q", true),
                                                               qRange, 1.0f, AudioParameterFloatAttributes().withStringFromValueFunction (one)));
        grp->addChild (std::make_unique<AudioParameterFloat>  (ParameterID { laneParamId (b, L, "gain"), 1 }, nm ("Gain", true),
                                                               NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
                                                               AudioParameterFloatAttributes().withStringFromValueFunction (oneDb)));
        grp->addChild (std::make_unique<AudioParameterChoice> (ParameterID { laneParamId (b, L, "slope"), 1 }, nm ("Slope", true), slopeItems, 1));
        grp->addChild (std::make_unique<AudioParameterBool>   (ParameterID { laneParamId (b, L, "byp"), 1 }, nm ("Bypass", false), false));
    }

    layout.add (std::move (grp));
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (int b = 0; b < kNumBands; ++b) addBand (layout, b);

    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "output", 1 }, "Output",
                                                             juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f,
                                                             juce::AudioParameterFloatAttributes().withLabel ("dB")));

    // Global phase mode + linear-phase quality (FIR length -> latency). Zero Latency = matched IIR
    // (minimum-phase, no delay, the track-EQ default). Natural Phase = mixed-phase FIR (φ=k·φ_min).
    // Linear Phase = FIR convolution (no phase shift, N/2 latency, for surgical band work).
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "phaseMode", 1 }, "Phase",
                                                              juce::StringArray { "Zero Latency", "Natural Phase", "Linear Phase" }, 0));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "lpQuality", 1 }, "Linear Quality",
                                                              juce::StringArray { "Low", "Medium", "High", "Very High", "Maximum" }, 1));   // default Medium — FabFilter-style ladder
    // Natural-phase blend: 0 = linear (flat phase, more latency/pre-ring) … 1 = minimum phase (no pre-ring,
    // ~0 latency, full phase shift). 0.5 = the mastering middle ground. Only bites in Natural Phase mode.
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "phaseAmount", 1 }, "Phase Amount",
                                                             juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.5f));
    return layout;
}

} // namespace tabby
