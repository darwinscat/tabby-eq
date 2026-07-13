// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BrandMark.h"   // tabby::brand — the stripe-cat fallback mark + Michroma wordmark
#include "Palette.h"

#include <cmath>
#include <functional>

//==============================================================================
// HeaderBrand — the FabFilter-style brand "blister": a rounded-bottom badge that protrudes a touch
// below the top toolbar, holding a LARGE Darwin's Cat mark + the "TabbyEQ" Michroma wordmark and a
// small "by Darwin's Cat" tagline under it:
//
//    ╭──────────────────────────╮
//    │  🐱   TabbyEQ             │
//    │       by Darwin's Cat    │
//    ╰──────────────────────────╯
//
// The editor slides it to the window centre when there's room (wide window), or anchors it left
// (narrow) — see the editor's resized(). The whole badge links to the product page with a soft
// accent halo on hover. The cat is a juce::Drawable set by the editor (BinaryData); if absent it
// falls back to the procedural stripe mark (tabby::brand::drawMark).
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

    // Width the badge needs at a given height: pads + cat square + gap + the wider text line.
    int preferredWidth (int height) const
    {
        const float h = (float) height;
        const float textW = juce::jmax (tabby::brand::textWidth (wordFont (h), kWord),
                                        tabby::brand::textWidth (tagFont (h),  kTag));
        return (int) std::ceil (kPadL + catSize (h) + kGap + textW + kPadR);
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
        const auto b = getLocalBounds().toFloat();
        const float h = b.getHeight();

        // --- the blister panel: flat top (flush with the toolbar), rounded bottom corners ---
        juce::Path panel;
        const float r = juce::jmin (11.0f, h * 0.28f);
        panel.startNewSubPath (b.getX(), b.getY());
        panel.lineTo (b.getRight(), b.getY());
        panel.lineTo (b.getRight(), b.getBottom() - r);
        panel.quadraticTo (b.getRight(), b.getBottom(), b.getRight() - r, b.getBottom());
        panel.lineTo (b.getX() + r, b.getBottom());
        panel.quadraticTo (b.getX(), b.getBottom(), b.getX(), b.getBottom() - r);
        panel.closeSubPath();

        g.setGradientFill (juce::ColourGradient (tabby::palette::panel().brighter (0.06f), 0.0f, b.getY(),
                                                 tabby::palette::panel().darker (0.25f),   0.0f, b.getBottom(), false));
        g.fillPath (panel);
        g.setColour ((hover ? accent : tabby::palette::text()).withAlpha (hover ? 0.55f : 0.14f));
        g.strokePath (panel, juce::PathStrokeType (1.0f));   // subtle rim, accent-tinted on hover

        // --- content: cat square + wordmark/tagline, vertically centred in the toolbar band ---
        auto content = b.reduced (0.0f, 1.0f);
        content.removeFromBottom (h * 0.12f);                // keep clear of the rounded bottom
        const float cs = catSize (h);
        auto catArea = content.removeFromLeft (kPadL + cs).withTrimmedLeft (kPadL);
        catArea = catArea.withSizeKeepingCentre (cs, cs);
        if (catLogo != nullptr)
            catLogo->drawWithin (g, catArea, juce::RectanglePlacement::centred, hover ? 1.0f : 0.92f);
        else
            tabby::brand::drawMark (g, catArea, hover);

        const float x  = catArea.getRight() + kGap;
        const auto  wf = wordFont (h);
        const auto  tf = tagFont (h);
        // Baseline group: the wordmark sits above centre, the tagline just below it.
        const float cy       = content.getCentreY();
        const float baseline = cy + wf.getHeight() * 0.16f;

        g.setFont (wf);
        g.setColour (hover ? juce::Colours::white : juce::Colour (0xffeef0f6));
        g.drawSingleLineText (kWord, juce::roundToInt (x), juce::roundToInt (baseline));

        g.setFont (tf);
        g.setColour (hover ? accent.brighter (0.30f) : accent);
        g.drawSingleLineText (kTag, juce::roundToInt (x), juce::roundToInt (baseline + tf.getHeight() * 0.95f));
    }

private:
    static float catSize (float h) { return h * 0.72f; }
    static float wordH   (float h) { return h * 0.40f; }    // "TabbyEQ"
    static float tagH    (float h) { return h * 0.175f; }   // "by Darwin's Cat"

    juce::Font wordFont (float h) const { return tabby::brand::wordmarkFont (wordmarkTypeface, wordH (h)); }
    juce::Font tagFont  (float h) const { return tabby::brand::wordmarkFont (wordmarkTypeface, tagH (h)); }

    bool hover = false;
    static constexpr float kPadL = 10.0f, kGap = 9.0f, kPadR = 14.0f;
    const juce::String kWord = "TabbyEQ";
    const juce::String kTag  = juce::String::fromUTF8 ("by Darwin\xe2\x80\x99s Cat");

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBrand)
};
