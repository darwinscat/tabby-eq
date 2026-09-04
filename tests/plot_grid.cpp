// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.
//
// JUCE-free theory unit for the eqview PlotGrid (src/eqview/PlotGrid.h) — WHERE a logarithmic
// frequency axis puts its ticks, which of them carry a number, and how a linear (dB) scale thins
// its numbers. House policy: expectations from THEORY, not from the code — the ruler is the set
// {k · 10^n}, a decade is a constant width on a log axis, and a ladder's promise is a MEASURED
// pixel gap, checked here against PlotMap's own mapping rather than against the constants the
// header keeps.

#include "eqview/PlotGrid.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace g = eqview::grid;

static int failures = 0;
static void check (bool ok, const char* what)
{
    if (! ok) { std::printf ("FAIL: %s\n", what); ++failures; }
    else        std::printf ("ok:   %s\n", what);
}
static bool nearEq (double a, double b, double tol) { return std::abs (a - b) <= tol; }

static eqview::PlotMap mapOf (float width, double lo = 20.0, double hi = 28000.0)
{
    eqview::PlotMap pm;
    pm.width = width; pm.freqMin = lo; pm.freqMax = hi;
    return pm;
}

// Every tick the header hands out, in order.
static std::vector<std::pair<double, int>> ticksOf (const eqview::PlotMap& pm)
{
    std::vector<std::pair<double, int>> out;
    g::forEachTick (pm, [&out] (double f, int step) { out.emplace_back (f, step); });
    return out;
}

int main()
{
    //==========================================================================================
    // The ruler itself: the set {k · 10^n} ∩ [freqMin, freqMax], ascending, k ∈ 1…9, and nothing
    // outside the map's own range (a tick off the axis would be drawn clamped, i.e. in the wrong
    // place). 20 Hz … 28 kHz holds 20…90, 100…900, 1k…9k, 10k and 20k.
    {
        const auto pm = mapOf (800.0f);
        const auto t  = ticksOf (pm);

        check (t.size() == 8 + 9 + 9 + 2, "20 Hz..28 kHz yields exactly 28 ticks");

        bool ascending = true, inRange = true, stepSane = true, isDecadeMultiple = true;
        for (size_t i = 0; i < t.size(); ++i)
        {
            if (i > 0 && ! (t[i].first > t[i - 1].first)) ascending = false;
            if (t[i].first < pm.freqMin || t[i].first > pm.freqMax) inRange = false;
            if (t[i].second < 1 || t[i].second > 9) stepSane = false;

            // f / step must be a whole power of ten — that IS the definition of the ruler.
            const double decade = t[i].first / (double) t[i].second;
            if (! nearEq (std::log10 (decade), std::round (std::log10 (decade)), 1e-9))
                isDecadeMultiple = false;
        }
        check (ascending,        "ticks come out strictly ascending");
        check (inRange,          "no tick falls outside [freqMin, freqMax]");
        check (stepSane,         "every step is in 1..9");
        check (isDecadeMultiple, "every tick is step x a whole power of ten");

        check (t.front().first == 20.0 && t.front().second == 2, "first tick is 20 Hz (step 2)");
        check (t.back().first == 20000.0 && t.back().second == 2, "last tick is 20 kHz (step 2)");

        // A degenerate map must draw nothing rather than loop or divide by zero.
        eqview::PlotMap bad; bad.freqMin = 0.0; bad.freqMax = 0.0;
        check (ticksOf (bad).empty(), "a degenerate range yields no ticks at all");
    }

    //==========================================================================================
    // Decade width: on a log axis every decade is the SAME width, so the header's one number must
    // equal the measured distance between f and 10f — at any f.
    {
        const auto pm = mapOf (800.0f);
        const float d = g::decadeWidth (pm);
        bool same = true;
        for (double f : { 20.0, 63.0, 100.0, 440.0, 1000.0, 2500.0 })
            if (! nearEq ((double) (pm.freqToX (f * 10.0) - pm.freqToX (f)), (double) d, 1e-3))
                same = false;
        check (same, "decadeWidth == the measured px distance from f to 10f, wherever f sits");
        check (nearEq ((double) d, 800.0 / std::log10 (28000.0 / 20.0), 1e-3),
               "decadeWidth == width / log10(fmax/fmin)");
    }

    //==========================================================================================
    // Weights: the decade line is step 1 alone — the joints of the scale. The default caption set
    // is the 1-2-5 series, and it is exactly the Sparse rung.
    {
        bool decadeOk = true, captionOk = true;
        for (int s = 1; s <= 9; ++s)
        {
            if (g::isDecade (s) != (s == 1)) decadeOk = false;
            if (g::isCaptioned (s) != (s == 1 || s == 2 || s == 5)) captionOk = false;
            if (g::isCaptioned (s) != g::isLabelled (s, g::LabelSet::Sparse)) captionOk = false;
        }
        check (decadeOk,  "isDecade is true for step 1 and nothing else");
        check (captionOk, "isCaptioned == the 1-2-5 series == LabelSet::Sparse");

        // The dense rung is a SUPERSET of the default one: widening a window may add numbers, it
        // must never take one away.
        bool superset = true;
        for (int s = 1; s <= 9; ++s)
            if (g::isLabelled (s, g::LabelSet::Sparse) && ! g::isLabelled (s, g::LabelSet::Dense))
                superset = false;
        check (superset, "Dense contains every number Sparse shows");
        check (g::isLabelled (1, g::LabelSet::Decades) && ! g::isLabelled (2, g::LabelSet::Decades)
               && ! g::isLabelled (3, g::LabelSet::Decades),
               "Decades shows the decade and nothing else");
        check (g::isLabelled (1, g::LabelSet::HalfDecades) && g::isLabelled (3, g::LabelSet::HalfDecades)
               && ! g::isLabelled (2, g::LabelSet::HalfDecades),
               "HalfDecades is the 1-3 series (log-even: log10 3 ~ half a decade)");
    }

    //==========================================================================================
    // THE PROMISE of the frequency ladder: whatever rung it picks, the closest pair of NUMBERS on
    // the axis is at least minGapPx apart — measured through PlotMap, not asserted from constants.
    // Below the last rung (Decades) there is nothing sparser to fall back to, so it is exempt.
    {
        const double minGap = 50.0;
        bool kept = true, monotone = true;
        int  lastDensity = -1;
        float widthAtFirstDense = 0.0f;

        for (float w = 120.0f; w <= 4000.0f; w += 20.0f)
        {
            const auto pm  = mapOf (w);
            const auto set = g::labelSet (g::decadeWidth (pm), minGap);

            // Denser rungs must only ever arrive as the axis WIDENS.
            const int density = set == g::LabelSet::Dense ? 3 : set == g::LabelSet::Sparse ? 2
                              : set == g::LabelSet::HalfDecades ? 1 : 0;
            if (density < lastDensity) monotone = false;
            lastDensity = density;
            if (density == 3 && widthAtFirstDense == 0.0f) widthAtFirstDense = w;

            if (set == g::LabelSet::Decades)
                continue;

            float prevX = -1.0e9f;
            for (const auto& [f, step] : ticksOf (pm))
            {
                if (! g::isLabelled (step, set))
                    continue;
                const float x = pm.freqToX (f);
                if (prevX > -1.0e8f && (double) (x - prevX) < minGap - 1e-3)
                    kept = false;
                prevX = x;
            }
        }
        check (kept,     "every chosen rung keeps minGapPx between neighbouring numbers");
        check (monotone, "the rung never gets denser as the axis narrows");
        check (widthAtFirstDense > 1000.0f && widthAtFirstDense < 1120.0f,
               "the 1-2-3-5-7 row arrives around an ~1080 px axis (50 px gap / log10 1.4)");
    }

    //==========================================================================================
    // The linear (dB) ladder. Its promises: the step is always a WHOLE multiple of the scale's own
    // step (a caption must land on a grid line), it never gets coarser as the scale gets taller,
    // and it clears the gap whenever any rung can.
    {
        const double base = 6.0, minGap = 25.0;
        bool multiple = true, monotone = true, clears = true;
        double prev = 1.0e9;

        for (double px = 0.2; px <= 40.0; px += 0.1)
        {
            const double s = g::labelStep (base, px, minGap);
            const double m = s / base;
            if (! nearEq (m, std::round (m), 1e-9) || m < 1.0 || m > 6.0) multiple = false;
            if (s > prev + 1e-9) monotone = false;
            prev = s;
            if (base * 6.0 * px >= minGap && s * px < minGap - 1e-9) clears = false;
        }
        check (multiple, "every rung is a whole 1..6 multiple of the scale's own step");
        check (monotone, "the step never grows as the scale gets more room");
        check (clears,   "the chosen step clears minGapPx whenever a rung can");

        check (nearEq (g::labelStep (6.0, 8.0, 25.0),  6.0,  1e-9), "roomy gain scale keeps its 6 dB step");
        check (nearEq (g::labelStep (6.0, 3.0, 25.0),  12.0, 1e-9), "a short plot doubles it to 12 dB");
        check (nearEq (g::labelStep (10.0, 2.0, 25.0), 20.0, 1e-9), "the level column goes 10 -> 20 dB");
        check (nearEq (g::labelStep (10.0, 1.0, 25.0), 30.0, 1e-9), "...then 30 dB");
        check (nearEq (g::labelStep (10.0, 0.1, 25.0), 60.0, 1e-9), "...and bottoms out at the x6 rung");

        // A degenerate scale must hand back something drawable rather than 0 or infinity.
        check (nearEq (g::labelStep (6.0, 0.0, 25.0), 6.0, 1e-9), "zero px/unit falls back to the base step");
        check (nearEq (g::labelStep (0.0, 8.0, 25.0), 0.0, 1e-9), "a zero base is handed straight back");
    }

    std::printf (failures == 0 ? "\nAll PlotGrid checks passed.\n" : "\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
