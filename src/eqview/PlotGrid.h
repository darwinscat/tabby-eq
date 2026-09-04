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

// The steps that carry a caption: 1, 2 and 5 per decade — 20 · 50 · 100 · 200 · 500 · 1k · 2k · 5k…
// (the 1-2-5 series every audio plot in the family labels). A captioned step always gets its line,
// however narrow the plot: a number with no line under it is not a scale. The rest is fine ruler.
constexpr bool isCaptioned (int step) noexcept { return step == 1 || step == 2 || step == 5; }

// One decade in pixels. The X map is logarithmic, so every decade is the same width — one number
// that tells the caller whether the ruler between the captions would still be readable.
inline float decadeWidth (const PlotMap& pm) noexcept
{
    const double span = std::log10 (pm.freqMax / pm.freqMin);
    return span > 0.0 ? (float) ((double) pm.width / span) : 0.0f;
}

// Walks the ruler inside the map's own range, low to high: fn (frequency, step), where step is the
// 1…9 position inside its decade — the caller weights it through isDecade / isCaptioned rather than
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
