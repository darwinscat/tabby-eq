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
    float                 toolbarBottom = 30.0f;   // the toolbar's bottom line = the bell's "0" (set by the editor)

    // Suppress the left skirt (straight left cut) when the badge is flush against the window edge.
    void setLeftFlush (bool f) { if (leftFlush != f) { leftFlush = f; repaint(); } }
    bool isLeftFlush() const noexcept { return leftFlush; }

    // Append the bulge bottom profile (left flat run + dip + right flat run) to `p`, in ABSOLUTE
    // coords, continuing from the current point at (x0, y0). Ends at (x0 + w, y0). Shared by this
    // badge's own fill AND the full-width toolbar underline overlay, so the line is ONE path.
    static void appendBottomLine (juce::Path& p, float x0, float w, float y0, float yMax, bool leftFlush)
    {
        const float dipL = x0 + (leftFlush ? 0.0f : kEndFlat);
        const float dipR = x0 + w - kEndFlat;
        if (leftFlush)
            p.lineTo (x0, yMax);                            // straight cut down at the window edge
        else
        {
            p.lineTo (dipL, y0);                            // left flat run
            for (int i = 1; i <= kN; ++i)                   // left S-transition
                p.lineTo (dipL + (float) i / (float) kN * kTransW, y0 + (yMax - y0) * smoothstep ((float) i / (float) kN));
        }
        p.lineTo (dipR - kTransW, yMax);                    // flat plateau under the content
        for (int i = 1; i <= kN; ++i)                       // right S-transition
            p.lineTo (dipR - kTransW + (float) i / (float) kN * kTransW, y0 + (yMax - y0) * smoothstep (1.0f - (float) i / (float) kN));
        p.lineTo (x0 + w, y0);                              // right flat run
    }
    static constexpr float kLineY () { return 30.0f; }      // toolbar bottom (matches the editor)

    // Width the badge needs: both end-flats + both S-transitions (outside the content) + the content
    // (cat + gap + EQ mark + gap + "TabbyEQ" wordmark). The plateau spans under the whole logo+text.
    int preferredWidth (int height) const
    {
        const float h = (float) height;
        return (int) std::ceil (2.0f * (kEndFlat + kTransW) + kPadL + markSize (h) + kGap + markSize (h) * 0.94f
                                + kGap + tabby::brand::textWidth (wordFont (h), kWord) + kPadR);
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

        // --- the blister is a downward BULGE of the toolbar itself (SAME colour, not an overlaid
        // panel): the toolbar's bottom line DIPS down to a flat plateau under the logo+text and
        // rises back, blending in by TANGENT (zero-slope smoothstep) with no corner. Here we only
        // FILL the bulge (bg); the LINE is drawn ONCE, full width, by the editor's ToolbarUnderline
        // overlay (using the SAME appendBottomLine), so there is no line-junction to mismatch.
        const float y0   = toolbarBottom;
        const float yMax = H - 1.0f;

        juce::Path fill;
        fill.startNewSubPath (0.0f, leftFlush ? yMax : y0);
        appendBottomLine (fill, 0.0f, W, y0, yMax, leftFlush);   // dip profile, ends at (W, y0)
        fill.lineTo (W, 0.0f);
        fill.lineTo (0.0f, 0.0f);
        fill.closeSubPath();
        g.setColour (tabby::palette::bg());
        g.fillPath (fill);

        // --- content: cat mark + EQ stripe mark (raised) + "TabbyEQ" wordmark ---
        // The marks are taller than the toolbar band and use the bulge below; a small top margin
        // keeps the cat's ears off the top edge.
        const float ms = markSize (H);
        const float cy = kTopPad + ms * 0.5f;              // marks hang a hair below the top edge
        float x = (leftFlush ? kPadL : kEndFlat + kTransW + kPadL);   // the transition zone lives left of the content

        auto catArea = juce::Rectangle<float> (x, cy - ms * 0.5f, ms, ms);
        if (catLogo != nullptr)
            catLogo->drawWithin (g, catArea, juce::RectanglePlacement::centred, hover ? 1.0f : 0.92f);
        else
            tabby::brand::drawMark (g, catArea, hover);   // fallback
        x += ms + kGap;

        const float ems = ms * 0.94f;                      // the EQ mark, a touch smaller and slightly RAISED
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

    // Smoothstep 0→1 with zero slope at both ends — the sigmoid that blends the plateau into the
    // toolbar line without a corner (Oleh's soft EQ-curve transition).
    static float smoothstep (float t) { t = juce::jlimit (0.0f, 1.0f, t); return t * t * (3.0f - 2.0f * t); }

    juce::Font wordFont (float h) const { return tabby::brand::wordmarkFont (wordmarkTypeface, wordH (h)); }

    bool hover = false, leftFlush = false;
    static constexpr int   kN       = 30;                   // dip curve resolution
    static constexpr float kTransW  = 30.0f;                // width of each soft S-transition
    static constexpr float kEndFlat = 12.0f;               // flat run along the toolbar line at each end (tangent)
    static constexpr float kTopPad  = 3.5f;                // top margin so the cat's ears clear the top edge
    static constexpr float kPadL = 5.0f, kGap = 9.0f, kPadR = 5.0f;   // the rise butts up close to the wordmark
    const juce::String kWord = "TabbyEQ";

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBrand)
};
