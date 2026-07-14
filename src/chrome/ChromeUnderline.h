// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BrandBlister.h"
#include "ChromeMetrics.h"

namespace tabby::chrome
{

//==============================================================================
// ChromeUnderline — the toolbar's bottom line as SHELL state: ONE continuous path across the FULL
// width — flat, DIPPING under the brand blister (the same BrandBlister::appendBottomLine the frame
// fills with), flat again. It is a top-most, MOUSE-TRANSPARENT overlay owned by the shell, so the
// border is one uniform hairline that just happens to dip, with no line-junction to mismatch in
// thickness/position.
//
// It reads the blister's live bounds through getLocalArea (so it never assumes a shared origin) and
// the shell repaints it whenever the blister MOVES (BrandBlister::onMoved). The blister is always
// laid out at its full preferred width — even at negative x — so the left cut is produced purely by
// the WINDOW clip; the overlay follows the blister off-screen without intersecting the viewport.
class ChromeUnderline final : public juce::Component
{
public:
    ChromeUnderline (const BrandBlister& b, const ChromeMetrics& m, const ChromeTheme& t)
        : blister (b), metrics (m), theme (t) { setInterceptsMouseClicks (false, false); }

    void paint (juce::Graphics& g) override
    {
        const auto  bb   = getLocalArea (&blister, blister.getLocalBounds());   // blister → overlay coords
        const float ly   = metrics.barHeight;
        const float yMax = (float) bb.getBottom() - 1.0f;   // the blister's plateau depth

        // One continuous line. When the blister has slid its left edge OFF-SCREEN (bb.getX() < 0),
        // start the path at the blister's (negative) left edge so the dip is uninterrupted — the
        // window clips the off-screen part into a straight rectangular cut. Otherwise the far-left
        // flat run starts at the window edge.
        juce::Path p;
        p.startNewSubPath ((float) juce::jmin (0, bb.getX()), ly);
        if (bb.getX() > 0)
            p.lineTo ((float) bb.getX(), ly);                // flat, left of the blister
        BrandBlister::appendBottomLine (p, (float) bb.getX(), (float) bb.getWidth(), ly, yMax);
        p.lineTo ((float) getWidth(), ly);                   // flat, right of the blister
        g.setColour (theme.underline);
        g.strokePath (p, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
    }

private:
    const BrandBlister&  blister;
    const ChromeMetrics& metrics;
    const ChromeTheme&   theme;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChromeUnderline)
};

} // namespace tabby::chrome
