// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BrandMark.h"   // tabby::brand — the stripe-spectrum EQ mark + Michroma wordmark
#include "Palette.h"
#include "../chrome/BrandBlister.h"   // tabby::chrome::BlisterMark + the frame's public skirt constants

#include <cmath>
#include <functional>

namespace tabby::ui
{

//==============================================================================
// TabbyMark — TabbyEQ's brand mark, the PRODUCT half of the header blister (the frame is
// tabby::chrome::BrandBlister). It draws the THREE marks inside the badge and owns the hover +
// click-to-website; the frame fills the bulge behind it.
//
//   (  🐱  ▮▮▮  TabbyEQ  )     🐱 = Darwin's Cat mark   ▮▮▮ = the stripe EQ mark   + Michroma wordmark
//
// The mark covers the WHOLE blister (the frame sizes it to the full badge), so the entire badge is
// the clickable link with a hand cursor — behaviour verbatim from the old HeaderBrand. It reports
// the blister's preferred width via BlisterMark::preferredWidth (the historical expression, order
// preserved) and draws its content starting at the frame's contentLeftOffset.
class TabbyMark final : public tabby::chrome::BlisterMark,
                        public juce::SettableTooltipClient
{
public:
    TabbyMark() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    juce::Drawable*       catLogo = nullptr;       // Darwin's Cat mark — not owned (editor holds it)
    juce::Typeface::Ptr   wordmarkTypeface;        // Michroma — set by the editor (BinaryData)
    juce::Colour          accent { tabby::palette::violet() };   // (unused today — do NOT wire; that would be a visible change)
    std::function<void()> onLaunch;

    // Preferred FULL blister width: both skirts (frame) + both inner pads (frame) + the content (cat
    // + gap + EQ mark + gap + "TabbyEQ" wordmark). Relocated VERBATIM from HeaderBrand — the order of
    // additions is pixel-critical; the frame's skirt/pad constants are referenced by name.
    int preferredWidth (int height) const override
    {
        using B = tabby::chrome::BrandBlister;
        const float h = (float) height;
        return (int) std::ceil (2.0f * (B::kEndFlat + B::kTransW) + B::kPadL + markSize (h) + kGap + markSize (h) * 0.94f
                                + kGap + tabby::brand::textWidth (wordFont (h), kWord) + B::kPadR);
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
        // The frame (BrandBlister) has already filled the bulge; the mark only draws the content:
        // cat mark + EQ stripe mark (raised) + "TabbyEQ" wordmark. The marks are taller than the
        // toolbar band and use the bulge below; a small top margin keeps the cat's ears off the top.
        const float H = (float) getHeight();
        const float ms = markSize (H);
        const float cy = kTopPad + ms * 0.5f;                          // marks hang a hair below the top edge
        float x = tabby::chrome::BrandBlister::contentLeftOffset();     // ALWAYS fixed — the left skirt lives to its left (may be off-screen)

        auto catArea = juce::Rectangle<float> (x, cy - ms * 0.5f, ms, ms);
        if (catLogo != nullptr)
            catLogo->drawWithin (g, catArea, juce::RectanglePlacement::centred, hover ? 1.0f : 0.92f);
        else
            tabby::brand::drawMark (g, catArea, hover);                // fallback
        x += ms + kGap;

        const float ems = ms * 0.94f;                                  // the EQ mark, a touch smaller and slightly RAISED
        tabby::brand::drawMark (g, juce::Rectangle<float> (x, cy - ems * 0.5f - ms * 0.07f, ems, ems), hover);
        x += ems + kGap;

        const auto  wf = wordFont (H);
        const float baseline = cy + (wf.getAscent() - wf.getDescent()) * 0.5f;
        g.setFont (wf);
        g.setColour (hover ? juce::Colours::white : juce::Colour (0xffeef0f6));
        g.drawSingleLineText (kWord, juce::roundToInt (x), juce::roundToInt (baseline));
    }

private:
    static float markSize (float h) { return h * 0.78f; }   // cat + EQ mark squares (bigger)
    static float wordH    (float h) { return h * 0.44f; }   // "TabbyEQ"

    juce::Font wordFont (float h) const { return tabby::brand::wordmarkFont (wordmarkTypeface, wordH (h)); }

    bool hover = false;
    static constexpr float kTopPad = 3.5f;              // top margin so the cat's ears clear the top edge
    static constexpr float kGap    = 9.0f;              // the rise butts up close to the wordmark
    const juce::String kWord = "TabbyEQ";

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TabbyMark)
};

} // namespace tabby::ui
