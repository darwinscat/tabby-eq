// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BrandMark.h"   // tabby::brand — the stripe-cat mark + Michroma wordmark
#include "Palette.h"

#include <cmath>
#include <functional>

//==============================================================================
// Clickable header brand (the orbitcab HeaderBrand pattern — same font, same byline, the LOGO
// differs):
//   [stripe-cat mark]  TabbyEQ     by Darwin's Cat
//                                  Semantic EQ
// The mark sits left of the "TabbyEQ" Michroma wordmark; a two-line block follows — "by Darwin's
// Cat" (accent) over "Semantic EQ" (dim), right-aligned to each other, the bottom line sharing
// the wordmark's baseline. The whole strip links to the product page with a soft accent halo on
// hover, and sizes itself to its content.
//==============================================================================
class HeaderBrand final : public juce::Component,
                          public juce::SettableTooltipClient
{
public:
    HeaderBrand() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    juce::Typeface::Ptr   wordmarkTypeface;        // Michroma — set by the editor (BinaryData)
    juce::Colour          accent { tabby::palette::violet() };
    std::function<void()> onLaunch;

    // Width the strip needs: mark square + wordmark + the wider subtitle line.
    int preferredWidth (int height) const
    {
        const float h    = (float) height;
        const float subW = tabby::brand::textWidth (font (subHeight (h)), kSub);
        return height + kGap
             + (int) std::ceil (tabby::brand::textWidth (font (wordHeight (h)), kWord))
             + kSubGap + (int) std::ceil (subW) + kPadR;
    }

    void mouseEnter (const juce::MouseEvent&) override { hover = true;  repaint(); }
    void mouseExit  (const juce::MouseEvent&) override { hover = false; repaint(); }
    void mouseUp    (const juce::MouseEvent& e) override
    {
        if (onLaunch && getLocalBounds().contains (e.getPosition()))
            onLaunch();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float h = (float) getHeight();

        if (hover)                                  // soft accent halo
        {
            g.setColour (accent.withAlpha (0.16f));
            g.fillRoundedRectangle (b, 8.0f);
        }

        // the stripe-cat mark (square at the strip height)
        tabby::brand::drawMark (g, b.removeFromLeft (h).reduced (3.0f), hover);
        b.removeFromLeft ((float) kGap);

        // Shared baseline so "TabbyEQ" and "Semantic EQ" sit on the SAME bottom line.
        const auto  wf = font (wordHeight (h));
        const float baseline = b.getCentreY() + (wf.getAscent() - wf.getDescent()) * 0.5f;
        float x = b.getX();

        g.setFont (wf);
        g.setColour (hover ? juce::Colours::white : juce::Colour (0xffeef0f6));
        g.drawSingleLineText (kWord, juce::roundToInt (x), juce::roundToInt (baseline));
        x += tabby::brand::textWidth (wf, kWord) + (float) kSubGap;

        // Two stacked lines right of the wordmark: "by Darwin's Cat" (accent, top) over
        // "Semantic EQ" (dim — bottom, SAME baseline as the wordmark), right-aligned together.
        const auto  sf   = font (subHeight (h));
        const auto  bf   = font (bylineHeight (h));
        const float subW = tabby::brand::textWidth (sf, kSub);
        const float bylW = tabby::brand::textWidth (bf, kByline);

        g.setFont (bf);
        g.setColour (hover ? accent.brighter (0.30f) : accent);
        g.drawSingleLineText (kByline, juce::roundToInt (x + subW - bylW),
                              juce::roundToInt (baseline - sf.getHeight() * 0.96f));

        g.setFont (sf);
        g.setColour (hover ? juce::Colour (0xffc8c8d2) : juce::Colour (0xff8a8a94));
        g.drawSingleLineText (kSub, juce::roundToInt (x), juce::roundToInt (baseline));
    }

private:
    static float wordHeight   (float h) { return h * 0.58f; }    // "TabbyEQ"
    static float subHeight    (float h) { return h * 0.30f; }    // "Semantic EQ" (bottom line)
    static float bylineHeight (float h) { return h * 0.21f; }    // "by Darwin's Cat" (top line)

    juce::Font font (float height) const { return tabby::brand::wordmarkFont (wordmarkTypeface, height); }

    bool hover = false;
    static constexpr int kGap = 7, kSubGap = 14, kPadR = 12;
    const juce::String kWord   = "TabbyEQ";
    const juce::String kSub    = "Semantic EQ";
    const juce::String kByline = juce::String::fromUTF8 ("by Darwin\xe2\x80\x99s Cat");

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBrand)
};
