// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <felitronics/analysis/PlotMap.h>

namespace eqview { using felitronics::analysis::PlotMap; }

#include <cmath>

//==============================================================================
// PlotGrid — WHERE the frequency ruler's lines go (eqview layer, step 0 incubation). The full
// logarithmic ruler an audio plot is read against: every 1…9 step of every decade, with the
// captioned ones (1, 2, 5 → 20/50/100/200/500/1k…) called out, so the eye can see that the axis is
// logarithmic instead of having to remember it. Ten lonely captioned lines make a log axis look
// like a linear one with odd labels; the ruler between them is what says "this decade is squeezed".
//
// JUCE-free and paint-free on purpose — the layering rule of this incubator (see
// .private/STEP0-LAYERING-MAP.md): the calculation is separate from the painting, so a consumer
// that draws its own way (a waveform, a cab curve, a corner-EQ) reuses the RULE and not a picture.
// The painting joins the eqview PlotSurface layer when this graduates.
namespace eqview::grid
{

// The step that OPENS a decade — 100 · 1k · 10k. These are the only heavy lines on the ruler: they
// are the scale's own joints, and everything between them is one decade's worth of squeeze. (Weighting
// 50/200/500 as heavily made the axis read as an evenly-stepped one; Pro-Q draws the decades alone.)
constexpr bool isDecade (int step) noexcept { return step == 1; }

// How many numbers the axis can carry. A scale is read by its numbers, and how many fit is a
// question about PIXELS, not about taste: the same ladder that reads generously across a wide
// window crowds into an unreadable row when the plot narrows. So the axis picks the densest rung
// its own width can hold — and picks it from a fixed ladder, so the numbers a reader learned to
// expect never move, they only appear and disappear.
enum class LabelSet
{
    Decades,      // 100 · 1k · 10k — the joints alone: the rung for a THUMBNAIL plot (a corner-EQ in
                  //                  a strip, a cab slot), where three numbers are all that fit
    HalfDecades,  // 1-3: 100 · 300 · 1k · 3k · 10k — evenly spaced in log (log10 3 ≈ half a decade)
    Sparse,       // 1-2-5: 20 · 50 · 100 · 200 · 500 · 1k…  — the family's default row
    Dense         // 1-2-3-5-7: + 30 · 70 · 300 · 700 · 3k · 7k — only where there is real room
};

constexpr bool isLabelled (int step, LabelSet set) noexcept
{
    switch (set)
    {
        case LabelSet::Dense:       return step == 1 || step == 2 || step == 3 || step == 5 || step == 7;
        case LabelSet::Sparse:      return step == 1 || step == 2 || step == 5;
        case LabelSet::HalfDecades: return step == 1 || step == 3;
        case LabelSet::Decades:     return step == 1;
    }
    return step == 1;
}

// The steps that carry a caption at the family's default density (1-2-5). A step in this set always
// gets its LINE, however narrow the plot: a number with no line under it is not a scale.
constexpr bool isCaptioned (int step) noexcept { return isLabelled (step, LabelSet::Sparse); }

// The tightest neighbouring gap inside one decade, in decades — the pair that decides whether a
// rung fits: dense is decided by 5→7, sparse by 1→2, half-decades by 1→3.
inline double tightestGap (LabelSet set) noexcept
{
    switch (set)
    {
        case LabelSet::Dense:       return 0.1461280356782382;   // log10(7/5)
        case LabelSet::Sparse:      return 0.3010299956639812;   // log10(2)
        case LabelSet::HalfDecades: return 0.4771212547196624;   // log10(3)
        case LabelSet::Decades:     return 1.0;
    }
    return 1.0;
}

// The densest rung whose closest pair of numbers still keeps `minGapPx` between them. `decadePx`
// comes from decadeWidth(); `minGapPx` is the caller's own type size plus the air it wants.
inline LabelSet labelSet (float decadePx, double minGapPx) noexcept
{
    for (auto set : { LabelSet::Dense, LabelSet::Sparse, LabelSet::HalfDecades })
        if ((double) decadePx * tightestGap (set) >= minGapPx)
            return set;
    return LabelSet::Decades;
}

// The same question for a LINEAR scale (dB up the side, and any other evenly-stepped axis). Three
// rungs — the scale's own step, doubled, tripled — and then one last rung at ×6 for the same
// thumbnail case the Decades rung serves horizontally. Every rung is a WHOLE multiple of `base`, so
// a caption never lands between the grid lines it is meant to name.
inline double labelStep (double base, double pxPerUnit, double minGapPx) noexcept
{
    if (! (base > 0.0) || ! (pxPerUnit > 0.0))
        return base;

    for (const double m : { 1.0, 2.0, 3.0 })
        if (base * m * pxPerUnit >= minGapPx)
            return base * m;
    return base * 6.0;
}

// One decade in pixels. The X map is logarithmic, so every decade is the same width — one number
// that tells the caller whether the ruler between the captions would still be readable.
inline float decadeWidth (const PlotMap& pm) noexcept
{
    const double span = std::log10 (pm.freqMax / pm.freqMin);
    return span > 0.0 ? (float) ((double) pm.width / span) : 0.0f;
}

// Walks the ruler inside the map's own range, low to high: fn (frequency, step), where step is the
// 1…9 position inside its decade — the caller weights it through isDecade / isLabelled rather than
// being handed a verdict, because a consumer with its own scale (a cab curve, a waveform) weighs
// them differently.
template <typename Fn>
void forEachTick (const PlotMap& pm, Fn&& fn)
{
    if (! (pm.freqMax > pm.freqMin) || pm.freqMin <= 0.0)
        return;

    for (double decade = std::pow (10.0, std::floor (std::log10 (pm.freqMin)));
         decade <= pm.freqMax; decade *= 10.0)
        for (int step = 1; step <= 9; ++step)
        {
            const double f = decade * (double) step;
            if (f >= pm.freqMin && f <= pm.freqMax)
                fn (f, step);
        }
}

} // namespace eqview::grid
