// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <teq/EqEngine.h>

//==============================================================================
// APVTS parameter layout for TabbyEQ. The automatable state is a plain N-band EQ — per band:
// on / type / freq / Q / gain / slope / swept — matching teq::BandParams. The semantic layer
// (source/trait/search→treat) is a UI macro on top (slice 3) that writes into these bands, so
// the DAW always sees a normal multiband EQ.
namespace tabby
{

inline constexpr int kNumBands = teq::EqEngine::kMaxBands;   // 24

// Parameter id for band `b`, e.g. bandId(0,"freq") -> "band0_freq".
juce::String bandId (int b, juce::StringRef suffix);

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

// APVTS "type" choice index -> teq::FilterType (choice order: Bell/LowShelf/HighShelf/HP/LP/BP).
teq::FilterType filterTypeFromChoice (int idx) noexcept;

} // namespace tabby
