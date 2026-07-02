// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_graphics/juce_graphics.h>

#include "ui/Palette.h"

//==============================================================================
// Venn glyphs for the placement lanes (LANES.md UI). Two overlapping circles (the L / R halves of the
// stereo field); each lane highlights a different region:
//   Left   = the left disc          Right = the right disc
//   Stereo = both discs (union)      Mid   = the lens (intersection)     Side = the two crescents
// FilterShapes.h-style vector drawing, shared by the strip's dropdown button (a composite "set glyph"
// of the enabled lanes) and the lane menu rows (one glyph each).
namespace tabby::venn
{
    struct Geom { juce::Point<float> lc, rc; float r; };

    inline Geom geom (juce::Rectangle<float> a) noexcept
    {
        a = a.reduced (1.5f);
        const float r = juce::jmin (a.getWidth() / 3.0f, a.getHeight() * 0.5f);
        const float d = r;                                    // centre distance -> a balanced lens
        const float cx = a.getCentreX(), cy = a.getCentreY();
        return { { cx - d * 0.5f, cy }, { cx + d * 0.5f, cy }, r };
    }

    inline juce::Path circle (juce::Point<float> c, float r)
    {
        juce::Path p; p.addEllipse (c.x - r, c.y - r, r * 2.0f, r * 2.0f); return p;
    }

    // Fill lane `lane`'s characteristic region within the two-circle venn `g` in the current colour.
    inline void fillRegion (juce::Graphics& gr, const Geom& g, int lane)
    {
        const juce::Path lp = circle (g.lc, g.r), rp = circle (g.rc, g.r);
        switch (lane)
        {
            case 1: gr.fillPath (lp); break;                                   // Left disc
            case 2: gr.fillPath (rp); break;                                   // Right disc
            case 3: { juce::Graphics::ScopedSaveState s (gr);                  // Mid = lens (L ∩ R)
                      gr.reduceClipRegion (lp); gr.fillPath (rp); } break;
            case 4: { juce::Path x; x.addEllipse (g.lc.x - g.r, g.lc.y - g.r, g.r * 2, g.r * 2);   // Side = crescents
                      x.addEllipse (g.rc.x - g.r, g.rc.y - g.r, g.r * 2, g.r * 2);
                      x.setUsingNonZeroWinding (false);                        // even-odd -> lens excluded
                      gr.fillPath (x); } break;
            default: { juce::Path u; u.addEllipse (g.lc.x - g.r, g.lc.y - g.r, g.r * 2, g.r * 2);  // Stereo = union
                       u.addEllipse (g.rc.x - g.r, g.rc.y - g.r, g.r * 2, g.r * 2);
                       gr.fillPath (u); } break;
        }
    }

    // One lane's glyph: dim outline circles + the lane's region filled in `col`. Callers pass NEUTRAL
    // text tones (one attribute = one carrier: the venn carries the SHAPE, the colour dot the colour —
    // coloured symbols read as mush at this size).
    inline void drawOne (juce::Graphics& gr, juce::Rectangle<float> area, int lane, juce::Colour col,
                         juce::Colour outline = tabby::palette::textDim())
    {
        const Geom g = geom (area);
        gr.setColour (col);
        fillRegion (gr, g, lane);
        gr.setColour (outline.withAlpha (0.45f));                             // context outline over the fill
        gr.strokePath (circle (g.lc, g.r), juce::PathStrokeType (0.9f));
        gr.strokePath (circle (g.rc, g.r), juce::PathStrokeType (0.9f));
    }

    // The strip button's composite: dim outline circles + every enabled lane's region washed in NEUTRAL
    // text tones (the active lane brighter). The colour lives solely in the button's active-lane dot
    // (and the canvas badges) — the venn only carries the enabled set's shape.
    inline void drawSet (juce::Graphics& gr, juce::Rectangle<float> area, unsigned enabledMask, int activeLane)
    {
        const Geom g = geom (area);
        gr.setColour (tabby::palette::textDim().withAlpha (0.5f));
        gr.strokePath (circle (g.lc, g.r), juce::PathStrokeType (0.9f));
        gr.strokePath (circle (g.rc, g.r), juce::PathStrokeType (0.9f));
        for (int lane = 0; lane < 5; ++lane)                                  // ST first (a wash), others over
            if (enabledMask & (1u << lane))
            {
                gr.setColour (tabby::palette::text().withAlpha (lane == activeLane ? 0.30f : 0.12f));   // fills stay
                // barely above the button tone — shape lives in the outlines (same rule as the menu rows)
                fillRegion (gr, g, lane);
            }
    }
}
