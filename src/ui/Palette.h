// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_graphics/juce_graphics.h>

//==============================================================================
// TabbyEQ colour palette — the single source of truth for the look. Two brand colours from
// darwinscat.com (shared with orbitcab): VIOLET (primary) + ORANGE (secondary). Everything the UI
// draws pulls from here, so re-skinning is one file. In the EQ they also carry meaning: a boost
// (above 0 dB) reads warm = orange, a cut (below 0 dB) reads cool = violet.
namespace tabby::palette
{
    // --- structure ---
    inline juce::Colour bg()        { return juce::Colour (0xff0e1014); }   // window / canvas
    inline juce::Colour panel()     { return juce::Colour (0xff1a1e24); }   // strips, bubbles
    inline juce::Colour grid()      { return juce::Colour (0x18ffffff); }   // faint grid lines
    inline juce::Colour gridZero()  { return juce::Colour (0x40ffffff); }   // the 0 dB line
    inline juce::Colour axisText()  { return juce::Colour (0x66ffffff); }   // freq / dB labels
    inline juce::Colour text()      { return juce::Colour (0xffd8d8d8); }
    inline juce::Colour textDim()   { return juce::Colour (0x99ffffff); }
    inline juce::Colour line()      { return juce::Colour (0xffe8eef5); }   // response-curve stroke
    inline juce::Colour spectrum()  { return juce::Colour (0xff7f93b5); }   // analyzer fill (cool grey-blue)

    // --- brand (two main colours) ---
    inline juce::Colour violet()    { return juce::Colour (0xff9170ff); }   // PRIMARY  — accent / cut (lifted for balance)
    inline juce::Colour violetLo()  { return juce::Colour (0xffb39bff); }   // hover / glow
    inline juce::Colour orange()    { return juce::Colour (0xffff8822); }   // SECONDARY — boost / warm
}
