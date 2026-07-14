// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace tabby::chrome
{

//==============================================================================
// ChromeBar — the toolbar's ZONE layout. Three zones share the top band:
//   • RigidCenter — a single contiguous run (blister + compare + preset) that translates AS ONE
//     rigid group and floats to the centre of the free band, sliding left (even past x=0) as the
//     window narrows, clamped so its first cell never crosses `minStartX` (FabFilter behaviour).
//   • RightPinned — a fixed run pinned to the right edge (gear · (i) · fullscreen), laid out
//     right-to-left; its left edge defines the free band the RigidCenter centres in.
//   • LeftPinned  — optional, pinned to the left edge (unused today; the shape is here for later).
//
// A cell is `{ component, region, fixedWidth, gapBefore, collapse }` (+ a placement box: an
// explicit `height` for the overhanging blister and `hInset/vInset` for the pinned glyph buttons).
// Cell width is STATE-INDEPENDENT — it never depends on active/hover/edited runtime state.
struct Cell
{
    enum class Region { RigidCenter, RightPinned, LeftPinned };

    juce::Component* component  = nullptr;
    Region           region     = Region::RigidCenter;
    int              fixedWidth = 0;
    int              gapBefore  = 0;      // gap before this cell within its run (toward the run's start)
    bool             collapse   = false;  // when true and the component is hidden, the cell + its gap vanish

    int              height     = 0;      // 0 = the bar height; a positive value = an overhanging cell (blister)
    int              hInset     = 0;      // horizontal inset inside the cell's slot (pinned glyph buttons)
    int              vInset     = 0;      // vertical inset inside the cell's slot
};

//==============================================================================
// The bar itself: a plain list of cells + the layout pass. Cheap to rebuild each resized().
struct ChromeBar
{
    std::vector<Cell> cells;

    // RigidCenter is ONE elastic block: every item gets an EQUAL gap, and the block width flexes
    // between minW and maxW (i.e. the even gap flexes minGap↔maxGap) as the window widens — capped at
    // maxW so a wide window just grows the side margins. Tunable by eye.
    int minGap = 0, maxGap = 80, edgeMargin = 0, rightGuard = 8;   // rightGuard: min gap kept before the RightPinned run

    void add (const Cell& c) { cells.push_back (c); }

    // Lay every cell out inside `bar` (the top strip). `minStartX` clamps the RigidCenter run's first
    // cell (so the blister's content never crosses the window edge). Returns the free band's right
    // edge (the RightPinned run's left edge, minus its leading gap) for callers that want it.
    int layout (juce::Rectangle<int> bar, int barHeight, int minStartX) const
    {
        const int y = bar.getY();
        auto place = [&] (const Cell& c, int x)
        {
            const int h = c.height > 0 ? c.height : barHeight;
            c.component->setBounds (juce::Rectangle<int> (x, y, c.fixedWidth, h).reduced (c.hInset, c.vInset));
        };
        auto live = [] (const Cell& c) { return c.component != nullptr && ! (c.collapse && ! c.component->isVisible()); };

        // ---- RightPinned: right-to-left from the bar's right edge; its left edge → bandRight ----
        int cursor = bar.getRight();
        for (auto it = cells.rbegin(); it != cells.rend(); ++it)
        {
            if (it->region != Cell::Region::RightPinned || ! live (*it)) continue;
            cursor -= it->fixedWidth;
            place (*it, cursor);
            cursor -= it->gapBefore;   // gap on the LEFT of this cell (toward the centre band)
        }
        const int bandRight = cursor;

        // ---- LeftPinned: left-to-right from the bar's left edge (optional; empty today) ----
        int leftX = bar.getX();
        for (const auto& c : cells)
        {
            if (c.region != Cell::Region::LeftPinned || ! live (c)) continue;
            leftX += c.gapBefore;
            place (c, leftX);
            leftX += c.fixedWidth;
        }

        // ---- RigidCenter: items EVENLY distributed inside one block whose width flexes [minW, maxW] ----
        // All gaps are equal and grow with the window up to maxGap; the block never exceeds maxW (the
        // surplus becomes side margin). Narrow packs to minGap and still slides/clips at minStartX (the
        // blister's cat butts the edge). Per-item gapBefore is unused in this region.
        int sumFixed = 0, nItems = 0;
        for (const auto& c : cells)
            if (c.region == Cell::Region::RigidCenter && live (c)) { sumFixed += c.fixedWidth; ++nItems; }

        if (nItems > 0)
        {
            const int bandLeft = leftX;   // right edge of any LeftPinned run (== bar.getX() when none)
            const int avail    = bandRight - bandLeft;
            const int nGaps    = juce::jmax (1, nItems - 1);
            const int minW     = sumFixed + nGaps * minGap;
            const int maxW     = sumFixed + nGaps * maxGap;
            // Cap the block at the free band (minus rightGuard) so it NEVER crosses into the RightPinned
            // corner even if a font metric bloats sumFixed past the tuned minimum — the gaps then
            // compress (can go below minGap) instead of the block hanging over the gear/(i)/fullscreen.
            const int blockW   = juce::jmin (juce::jlimit (minW, maxW, avail - 2 * edgeMargin), avail - rightGuard);
            const int gap      = nItems > 1 ? (blockW - sumFixed) / nGaps : 0;
            const int actualW  = sumFixed + nGaps * gap;   // integer-gap block width (no rounding drift)

            int x = juce::jmax (minStartX, bandLeft + (avail - actualW) / 2);
            bool first = true;
            for (const auto& c : cells)
            {
                if (c.region != Cell::Region::RigidCenter || ! live (c)) continue;
                if (! first) x += gap;
                first = false;
                place (c, x);
                x += c.fixedWidth;
            }
        }

        return bandRight;
    }
};

} // namespace tabby::chrome
