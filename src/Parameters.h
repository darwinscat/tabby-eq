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

// The Q a band of this type is BORN with: 0.707 for the Butterworth-clean families — HP/LP and the
// matched shelves (a shelf at the generic Q=1.0 default is a RESONANT shelf: it visibly dips past
// 0 dB above the corner and bulges below it — see MatchedBiquad's resonant-shelf notes) — 1.0 for
// everything else. Single source of truth for add, type-switch and the "+" ghost preview.
double defaultQFor (teq::FilterType t) noexcept;

// A type switch keeps a HAND-SET Q but re-seats an UNTOUCHED one: every ENABLED lane of `band`
// whose Q still sits at the OLD type's default is snapped to the NEW type's default (gestured, so
// the host records a proper edit). With Link Q on the point's width is ONE logical value: the snap
// is all-or-nothing across the enabled lanes and lands as a single write the processor's link
// mirror propagates (per-lane writes would race a just-made hand edit through the mirror FIFO).
// UI edit paths only, by design: host automation / generic-editor type changes must not make the
// plugin rewrite neighbouring parameters behind the host's back.
void snapQOnTypeSwitch (juce::AudioProcessorValueTreeState& apvts, int band, int oldChoiceIdx, int newChoiceIdx);

} // namespace tabby
