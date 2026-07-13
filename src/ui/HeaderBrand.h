// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BrandMark.h"   // tabby::brand — the stripe-spectrum EQ mark + Michroma wordmark
#include "Palette.h"

#include <cmath>
#include <functional>

//==============================================================================
// HeaderBrand — the FabFilter-style brand "blister": a streamlined badge that hangs from the top
// edge with soft EQ-curve skirts on both sides (NOT a rounded rectangle). It holds THREE marks:
//
//    ╱‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾╲
//   (  🐱  ▮▮▮  TabbyEQ    )      🐱  = Darwin's Cat mark   ▮▮▮ = the stripe EQ mark   + wordmark
//    ╲__________________╱
//
// The editor slides the whole toolbar group (blister + buttons) to keep it centred; when the window
// is squeezed to its minimum the group hits the left edge, the LEFT skirt disappears off-screen and
// the blister shows a straight left cut with the logo right there (setLeftFlush(true)) — the RIGHT
// skirt always streams. The badge links to the product page with a soft accent halo on hover.
//==============================================================================
class HeaderBrand final : public juce::Component,
                          public juce::SettableTooltipClient
{
public:
    HeaderBrand() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }

    juce::Drawable*       catLogo = nullptr;       // Darwin's Cat mark — not owned (editor holds it)
    juce::Typeface::Ptr   wordmarkTypeface;        // Michroma — set by the editor (BinaryData)
    juce::Colour          accent { tabby::palette::violet() };
    std::function<void()> onLaunch;

    // Suppress the left skirt (straight left cut) when the badge is flush against the window edge.
    void setLeftFlush (bool f) { if (leftFlush != f) { leftFlush = f; repaint(); } }

    // Width the badge needs: both skirt insets + cat + gap + EQ mark + gap + the "TabbyEQ" wordmark.
    int preferredWidth (int height) const
    {
        const float h = (float) height;
        return (int) std::ceil (2.0f * kSkirtInset + kPadL + markSize (h) + kGap + markSize (h) + kGap
                                + tabby::brand::textWidth (wordFont (h), kWord) + kPadR);
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
        const auto  b = getLocalBounds().toFloat();
        const float W = b.getWidth(), H = b.getHeight();

        // --- the streamlined blister: flat top, EQ-curve skirts sweeping down to a bottom plateau ---
        juce::Path panel;
        panel.startNewSubPath (0.0f, 0.0f);
        panel.lineTo (W, 0.0f);                                              // flat top
        panel.cubicTo (W, H * 0.52f, W - kSkirt * 0.5f, H, W - kSkirt, H);   // right skirt → plateau
        if (leftFlush)
            panel.lineTo (0.0f, H);                                          // straight left cut (skirt off-screen)
        else
        {
            panel.lineTo (kSkirt, H);                                        // plateau
            panel.cubicTo (kSkirt * 0.5f, H, 0.0f, H * 0.52f, 0.0f, 0.0f);   // left skirt
        }
        panel.closeSubPath();

        g.setGradientFill (juce::ColourGradient (tabby::palette::panel().brighter (0.07f), 0.0f, 0.0f,
                                                 tabby::palette::panel().darker (0.28f),   0.0f, H, false));
        g.fillPath (panel);
        g.setColour ((hover ? accent : tabby::palette::text()).withAlpha (hover ? 0.55f : 0.13f));
        g.strokePath (panel, juce::PathStrokeType (1.0f));                   // subtle rim, accent on hover

        // --- content: cat mark + EQ stripe mark + "TabbyEQ" wordmark, vertically centred ---
        const float cy = H * 0.46f;                       // a touch above centre — the plateau dips below
        const float ms = markSize (H);
        float x = (leftFlush ? kPadL : kSkirtInset + kPadL);

        auto catArea = juce::Rectangle<float> (x, cy - ms * 0.5f, ms, ms);
        if (catLogo != nullptr)
            catLogo->drawWithin (g, catArea, juce::RectanglePlacement::centred, hover ? 1.0f : 0.92f);
        else
            tabby::brand::drawMark (g, catArea, hover);   // fallback
        x += ms + kGap;

        tabby::brand::drawMark (g, juce::Rectangle<float> (x, cy - ms * 0.5f, ms, ms), hover);   // the EQ mark
        x += ms + kGap;

        const auto  wf = wordFont (H);
        const float baseline = cy + (wf.getAscent() - wf.getDescent()) * 0.5f;
        g.setFont (wf);
        g.setColour (hover ? juce::Colours::white : juce::Colour (0xffeef0f6));
        g.drawSingleLineText (kWord, juce::roundToInt (x), juce::roundToInt (baseline));
    }

private:
    static float markSize (float h) { return h * 0.62f; }   // cat + EQ mark squares
    static float wordH    (float h) { return h * 0.40f; }   // "TabbyEQ"

    juce::Font wordFont (float h) const { return tabby::brand::wordmarkFont (wordmarkTypeface, wordH (h)); }

    bool hover = false, leftFlush = false;
    static constexpr float kSkirt = 26.0f, kSkirtInset = 12.0f;   // skirt sweep width / content inset from it
    static constexpr float kPadL = 6.0f, kGap = 9.0f, kPadR = 16.0f;
    const juce::String kWord = "TabbyEQ";

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBrand)
};
