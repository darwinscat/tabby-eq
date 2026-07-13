// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// tabby::brand — the shared TabbyEQ brand primitives: the stripe-cat mark and the Michroma
// wordmark font. One source of truth so the editor header (HeaderBrand) and any popover render an
// IDENTICAL mark. Pure drawing helpers — the caller supplies the embedded typeface.
//
// The mark: five spectrum bars whose OUTER bars peak into cat ears, the centre (orange) bar is
// "the boosted band" — tabby stripes ARE the spectrum. Oleh's pick (logo-d1) from the 2026-07
// draft round; drawn procedurally from the SVG geometry (viewBox 0 0 100 100, source of truth in
// resources/logo/TabbyEQ-mark.svg), so it scales crisply at any size with no Drawable dependency.
//==============================================================================
namespace tabby::brand
{
    inline void drawMark (juce::Graphics& g, juce::Rectangle<float> area, bool hover = false)
    {
        const float s  = juce::jmin (area.getWidth(), area.getHeight()) / 100.0f;
        const float x0 = area.getCentreX() - 50.0f * s;
        const float y0 = area.getCentreY() - 50.0f * s;
        auto X = [&] (float v) { return x0 + v * s; };
        auto Y = [&] (float v) { return y0 + v * s; };

        const auto violet = juce::Colour (0xff9170ff).brighter (hover ? 0.15f : 0.0f);
        const auto lilac  = juce::Colour (0xffb39bff).brighter (hover ? 0.15f : 0.0f);
        const auto orange = juce::Colour (0xffff8a3d).brighter (hover ? 0.15f : 0.0f);

        juce::Path earL;   // "M12,88 V26 L24,46 V88 Z"
        earL.startNewSubPath (X (12), Y (88));
        earL.lineTo (X (12), Y (26));
        earL.lineTo (X (24), Y (46));
        earL.lineTo (X (24), Y (88));
        earL.closeSubPath();
        juce::Path earR;   // "M88,88 V26 L76,46 V88 Z"
        earR.startNewSubPath (X (88), Y (88));
        earR.lineTo (X (88), Y (26));
        earR.lineTo (X (76), Y (46));
        earR.lineTo (X (76), Y (88));
        earR.closeSubPath();
        g.setColour (violet);
        g.fillPath (earL);
        g.fillPath (earR);

        g.setColour (lilac);
        g.fillRoundedRectangle (X (28), Y (46), 12.0f * s, 42.0f * s, 2.0f * s);
        g.fillRoundedRectangle (X (60), Y (46), 12.0f * s, 42.0f * s, 2.0f * s);
        g.setColour (orange);
        g.fillRoundedRectangle (X (44), Y (40), 12.0f * s, 48.0f * s, 2.0f * s);
    }

    // The wordmark font — embedded Michroma (OFL) via the supplied typeface; bold system fallback if null.
    inline juce::Font wordmarkFont (juce::Typeface::Ptr typeface, float height)
    {
        if (typeface != nullptr)
            return juce::Font (juce::FontOptions().withHeight (height).withTypeface (typeface));
        return juce::Font (juce::FontOptions (height, juce::Font::bold));
    }

    // Rendered width of `s` in font `f` (for laying the wordmark + trailing text out by hand).
    inline float textWidth (const juce::Font& f, const juce::String& s)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (f, s, 0.0f, 0.0f);
        return ga.getBoundingBox (0, -1, true).getWidth();
    }
}
