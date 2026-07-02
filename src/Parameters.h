// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <teq/EqEngine.h>

//==============================================================================
// APVTS parameter layout for TabbyEQ — schema v3 (placement lanes). Each band ("point") carries a
// SHARED on/type/swept/bypass plus five independent placement lanes (ST / L / R / M / S), each with a
// full on/freq/Q/gain/slope/bypass set — matching teq::BandParams v2. The lanes are grouped per band
// (AudioProcessorParameterGroup, Pro-Q-style tree). The semantic layer (source/trait/search→treat) is a
// UI macro on top that writes into these bands, so the DAW always sees a normal multiband EQ.
namespace tabby
{

inline constexpr int kNumBands = teq::EqEngine::kMaxBands;   // 24
inline constexpr int kNumLanes = teq::kNumLanes;             // 5 — ST/L/R/M/S, order matches teq::Lane

// Parameter id for band `b`, e.g. bandId(0,"type") -> "band0_type".
juce::String bandId (int b, juce::StringRef suffix);

// Per-lane parameter id, e.g. laneParamId(0, 0, "freq") -> "band0_st_freq" (lane 0 = ST).
juce::String laneParamId (int b, int lane, juce::StringRef field);

// Lane key ("st","l","r","m","s") / display word ("Stereo","Left",…) for lane index 0..4 (teq::Lane order).
const char* laneKey  (int lane) noexcept;
const char* laneWord (int lane) noexcept;

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

// APVTS "type" choice index -> teq::FilterType (choice order: Bell/LowShelf/HighShelf/HP/LP/BP/Notch/AllPass/Tilt).
teq::FilterType filterTypeFromChoice (int idx) noexcept;

} // namespace tabby
