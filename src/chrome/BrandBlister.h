// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ChromeMetrics.h"

#include <cmath>
#include <functional>

namespace tabby::chrome
{

//==============================================================================
// BlisterMark — the PRODUCT's brand mark, drawn inside the blister frame. The product hands the
// frame a concrete mark (its logo + wordmark, e.g. tabby::ui::TabbyMark) and the mark reports the
// blister's preferred width for a given height. Kept abstract so BrandBlister stays product-free
// (ready to move to appkit) while the product mark stays in the product.
struct BlisterMark : juce::Component
{
    // Preferred FULL blister width (skirts INCLUDED) at a given blister height. The product owns
    // this because only it knows its content (marks + wordmark + font metrics); it references the
    // frame's public skirt constants so the two agree on a single width. The arithmetic order is
    // pixel-critical — the mark must relocate the historical expression verbatim.
    virtual int preferredWidth (int blisterHeight) const = 0;
};

//==============================================================================
// BrandBlister — the FabFilter-style brand blister as a FRAME (not a generic cell). It owns the
// downward BULGE of the toolbar, the soft EQ-curve SKIRTS at each end, the plateau depth and
// `contentLeftOffset`; a single ChromeMetrics.barHeight drives BOTH this fill AND the shell's
// underline stroke. The product hands it a MARK component; the frame FILLS the bulge (bg colour)
// and frames the mark on top. The continuous hairline is drawn ONCE by the shell overlay
// (ChromeUnderline) — never here — so there is no line junction to mismatch.
//
//    ╱‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾╲
//   (   <the product mark>  )
//    ╲__________________╱
//
// The frame is mouse-transparent to ITSELF (setInterceptsMouseClicks(false, true)); the mark child
// covers the whole badge and owns the hover + click, so the entire blister is the clickable link,
// exactly as the hand-rolled HeaderBrand was.
class BrandBlister final : public juce::Component
{
public:
    BrandBlister (const ChromeMetrics& m, const ChromeTheme& t) : metrics (m), theme (t)
    {
        setInterceptsMouseClicks (false, true);   // child-only hit-testing — the mark handles the click
    }

    // The product's mark. The frame does not own it (the product configures its assets); the frame
    // sizes it to the whole badge so the entire blister stays clickable.
    void setMark (BlisterMark* m)
    {
        if (mark == m) return;
        if (mark != nullptr) removeChildComponent (mark);
        mark = m;
        if (mark != nullptr) addAndMakeVisible (*mark);
        resized();
    }

    int preferredWidth (int height) const { return mark != nullptr ? mark->preferredWidth (height) : 0; }

    std::function<void()> onMoved;   // the shell repaints the underline when the blister moves

    //==============================================================================
    // Skirt geometry — the FRAME owns it. Public so the product mark can size itself to the frame
    // (its preferred width and content-left both fold these in). Values verbatim from HeaderBrand.
    static constexpr float kEndFlat = 12.0f;   // flat run along the toolbar line at each end (tangent)
    static constexpr float kTransW  = 30.0f;   // width of each soft S-transition
    static constexpr float kPadL = 5.0f, kPadR = 5.0f;   // inner pads between the skirt and the content

    // The content's left edge (the cat) sits this far from the blister's own left edge. The shell
    // clamps the group so the CAT never crosses the window edge: the blister then slides its left
    // skirt OFF-SCREEN (clipped by the window → a straight rectangular cut) with NO content jump.
    static constexpr float contentLeftOffset() { return kEndFlat + kTransW + kPadL; }

    // Append the bulge bottom profile (left flat run + dip + right flat run) to `p`, in ABSOLUTE
    // coords, continuing from the current point at (x0, y0). Ends at (x0 + w, y0). Shared by this
    // frame's own fill AND the full-width shell underline overlay, so the line is ONE path.
    // GEOMETRY VERBATIM — relocate unchanged (loop count, the -1.0f plateau, the order of ops).
    static void appendBottomLine (juce::Path& p, float x0, float w, float y0, float yMax)
    {
        const float dipL = x0 + kEndFlat;
        const float dipR = x0 + w - kEndFlat;
        p.lineTo (dipL, y0);                                // left flat run
        for (int i = 1; i <= kN; ++i)                       // left S-transition
            p.lineTo (dipL + (float) i / (float) kN * kTransW, y0 + (yMax - y0) * smoothstep ((float) i / (float) kN));
        p.lineTo (dipR - kTransW, yMax);                    // flat plateau under the content
        for (int i = 1; i <= kN; ++i)                       // right S-transition
            p.lineTo (dipR - kTransW + (float) i / (float) kN * kTransW, y0 + (yMax - y0) * smoothstep (1.0f - (float) i / (float) kN));
        p.lineTo (x0 + w, y0);                              // right flat run
    }

    void paint (juce::Graphics& g) override
    {
        const auto  b = getLocalBounds().toFloat();
        const float W = b.getWidth(), H = b.getHeight();

        // The blister is a downward BULGE of the toolbar itself (SAME colour, not an overlaid panel):
        // the toolbar's bottom line DIPS to a flat plateau under the mark and rises back, blending
        // in by TANGENT (zero-slope smoothstep) with no corner. Here we only FILL the bulge (bg);
        // the LINE is drawn ONCE, full width, by the shell's ChromeUnderline overlay (using the SAME
        // appendBottomLine), so there is no line-junction to mismatch.
        const float y0   = metrics.barHeight;   // ONE source (was HeaderBrand::toolbarBottom = 30)
        const float yMax = H - 1.0f;

        juce::Path fill;
        fill.startNewSubPath (0.0f, y0);
        appendBottomLine (fill, 0.0f, W, y0, yMax);   // dip profile, ends at (W, y0)
        fill.lineTo (W, 0.0f);
        fill.lineTo (0.0f, 0.0f);
        fill.closeSubPath();
        g.setColour (theme.fill);
        g.fillPath (fill);
    }

    void resized() override { if (mark != nullptr) mark->setBounds (getLocalBounds()); }
    void moved()   override { if (onMoved) onMoved(); }

private:
    static constexpr int kN = 30;   // dip curve resolution

    // Smoothstep 0→1 with zero slope at both ends — the sigmoid that blends the plateau into the
    // toolbar line without a corner (Oleh's soft EQ-curve transition).
    static float smoothstep (float t) { t = juce::jlimit (0.0f, 1.0f, t); return t * t * (3.0f - 2.0f * t); }

    const ChromeMetrics& metrics;
    const ChromeTheme&   theme;
    BlisterMark*         mark = nullptr;   // not owned — the product configures + outlives it

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrandBlister)
};

} // namespace tabby::chrome
