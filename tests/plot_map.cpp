// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.
//
// JUCE-free unit for the eqview PlotMap (src/eqview/PlotMap.h) — the single home of the plot's
// freq↔px / dB↔px math (step 0 of the eqview layering). Pins the axis anchors, the roundtrips, the
// clamping, and the DELIBERATE below-range overshoot of the curve scale, so splitting the display
// into layers can never silently move a pixel. Runs in the plugin-OFF core-tests ctest.

#include "eqview/PlotMap.h"

#include <cmath>
#include <cstdio>
#include <limits>

static int failures = 0;

static void check (bool ok, const char* what)
{
    if (! ok) { std::printf ("FAIL: %s\n", what); ++failures; }
    else        std::printf ("ok:   %s\n", what);
}

static bool nearEq (double a, double b, double tol) { return std::abs (a - b) <= tol; }

int main()
{
    // Geometry mirrors a real canvas: odd sizes, a reserved bottom strip (plotBottom < height).
    eqview::PlotMap m;
    m.width = 1237.0f; m.height = 741.0f; m.plotBottom = 657.0f;
    m.freqMin = 20.0; m.freqMax = 28000.0; m.dbRange = 12.0;
    m.specTop = 6.0; m.specBottom = -90.0;

    // --- X axis: anchors, clamping, roundtrip --------------------------------------------------
    check (nearEq (m.freqToX (m.freqMin), 0.0, 1e-6), "freqToX(freqMin) == 0");
    check (nearEq (m.freqToX (m.freqMax), m.width, 1e-3), "freqToX(freqMax) == width");
    check (nearEq (m.freqToX (5.0), 0.0, 1e-6), "freqToX clamps below freqMin");
    check (nearEq (m.freqToX (1.0e6), m.width, 1e-3), "freqToX clamps above freqMax");
    check (nearEq (m.xToFreq (-50.0f), m.freqMin, 1e-9), "xToFreq clamps left of the plot");
    check (nearEq (m.xToFreq (m.width * 2.0f), m.freqMax, 1e-6 * m.freqMax), "xToFreq clamps right of the plot");
    for (double f = 20.0; f <= 28000.0; f *= 1.5)
        check (nearEq (m.xToFreq (m.freqToX (f)), f, f * 1e-4), "x<->freq roundtrip across the axis");
    check (nearEq (m.xToFreq (m.freqToX (1000.0)), 1000.0, 0.1), "1 kHz roundtrip is tight");

    // --- curve Y scale: anchors, roundtrip, deliberate overshoot (NO clamping) ------------------
    check (nearEq (m.dbToY (0.0), 0.5 * m.plotBottom, 1e-6), "dbToY(0) == mid of the curve area");
    check (nearEq (m.dbToY (+m.dbRange), 0.0, 1e-6), "dbToY(+range) == top");
    check (nearEq (m.dbToY (-m.dbRange), m.plotBottom, 1e-6), "dbToY(-range) == plotBottom");
    check (nearEq (m.dbToY (-2.0 * m.dbRange), 1.5 * m.plotBottom, 1e-3), "below-range OVERSHOOTS past plotBottom (by design)");
    check (nearEq (m.dbToY (+2.0 * m.dbRange), -0.5 * m.plotBottom, 1e-3), "above-range overshoots past the top (by design)");
    for (double db = -30.0; db <= 30.0; db += 3.7)
        check (nearEq (m.yToDb (m.dbToY (db)), db, 1e-4), "y<->dB roundtrip incl. beyond the visible window");

    // --- spectrum Y scale: full height, clamped -------------------------------------------------
    check (nearEq (m.specDbToY (m.specTop), 0.0, 1e-6), "specDbToY(top) == 0");
    check (nearEq (m.specDbToY (m.specBottom), m.height, 1e-3), "specDbToY(bottom) == height");
    check (nearEq (m.specDbToY (m.specTop + 20.0), 0.0, 1e-6), "spectrum clamps above top");
    check (nearEq (m.specDbToY (m.specBottom - 40.0), m.height, 1e-3), "spectrum clamps below bottom");
    check (m.specDbToY (-20.0) > 0.0f && m.specDbToY (-20.0) < m.height, "mid dBFS lands inside the plot");

    // --- degenerate geometry: no div-by-zero, no NaN --------------------------------------------
    eqview::PlotMap z;   // all-zero width/height/plotBottom (component not yet resized)
    check (std::isfinite (z.xToFreq (10.0f)) && z.xToFreq (10.0f) >= z.freqMin, "zero width: xToFreq stays finite");
    check (std::isfinite (z.yToDb (10.0f)), "zero plotBottom: yToDb stays finite");
    check (nearEq (z.freqToX (1000.0), 0.0, 1e-9), "zero width: freqToX == 0");
    check (nearEq (z.specDbToY (-20.0), 0.0, 1e-9), "zero height: specDbToY == 0");

    eqview::PlotMap one = m; one.width = 1.0f; one.plotBottom = 1.0f;   // 1-px component (the max(1,·) guard's edge)
    check (std::isfinite (one.xToFreq (0.5f)) && one.xToFreq (0.5f) >= one.freqMin
                                              && one.xToFreq (0.5f) <= one.freqMax, "1-px width: xToFreq finite and in range");
    check (nearEq (one.yToDb (one.dbToY (3.0)), 3.0, 1e-4), "1-px plotBottom: y<->dB roundtrip still holds");

    // NaN passes THROUGH (both jlimit and std::clamp return the NaN operand) — it must not turn
    // into a bogus in-range coordinate, and it must not crash. Pin the pass-through.
    const double dnan = std::numeric_limits<double>::quiet_NaN();
    check (std::isnan (m.freqToX (dnan)), "NaN freq passes through freqToX as NaN");
    check (std::isnan (m.dbToY (dnan)),   "NaN dB passes through dbToY as NaN");

    // Huge magnitudes stay finite and obey the scales (curve overshoots linearly; spectrum clamps).
    check (std::isfinite (m.dbToY (1.0e6)) && std::isfinite (m.dbToY (-1.0e6)), "huge dB stays finite on the curve scale");
    check (nearEq (m.specDbToY (1.0e6), 0.0, 1e-6) && nearEq (m.specDbToY (-1.0e6), m.height, 1e-3), "huge dBFS clamps on the spectrum scale");

    std::printf (failures == 0 ? "PlotMap: all checks passed\n" : "PlotMap: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
