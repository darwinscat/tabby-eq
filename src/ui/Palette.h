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

    // --- placement-lane colours (LANES.md decision #5: roles locked, exact values tuned here) ---
    // A split point's nodes/curves/badges take these fixed per-lane colours. Tuned to the dark canvas
    // and the brand: readable (not neon), distinct hues, and clear of the per-band slot palette (badges
    // disambiguate the rare overlap). Roles are LOCKED — only the values are tuned.
    //   Stereo — the brand orange, i.e. the same hue as the hero composite (ST folds into every axis).
    //   Left   — a soft, cool near-white: the "reference" lane, calm and neutral.
    //   Right  — a clear red, warmer than the notch-type red and unmistakable next to orange.
    //   Mid    — an emerald green, calmer than the bell-type green so it reads as a role, not a filter.
    //   Side   — an azure blue, clear of the grey-blue spectrum fill and the violet accent.
    inline juce::Colour laneStereo() { return orange(); }                   // ST — brand orange (hero hue)
    inline juce::Colour laneLeft()   { return juce::Colour (0xffe6ebf2); }  // L  — soft cool white
    inline juce::Colour laneRight()  { return juce::Colour (0xffef5b6b); }  // R  — clear red
    inline juce::Colour laneMid()    { return juce::Colour (0xff46c98a); }  // M  — emerald green
    inline juce::Colour laneSide()   { return juce::Colour (0xff4f9bef); }  // S  — azure blue

    // Lane index (teq::Lane order: ST/L/R/M/S = 0..4) -> its fixed colour.
    inline juce::Colour lane (int idx)
    {
        switch (idx)
        {
            case 1:  return laneLeft();
            case 2:  return laneRight();
            case 3:  return laneMid();
            case 4:  return laneSide();
            default: return laneStereo();
        }
    }
}
