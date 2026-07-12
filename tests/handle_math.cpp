// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.
//
// JUCE-free theory unit for the eqview HandleMath (src/eqview/HandleMath.h) — node/whisker
// geometry + filter-type editing classification. House policy: expectations from THEORY, not the
// code — the log-linear Q<->bandwidth calibration's endpoints and monotonic invertibility, the
// slope-table index rules, the symmetric-in-log-frequency whisker geometry, and the type axioms.

#include "eqview/HandleMath.h"

#include <cmath>
#include <cstdio>

namespace h = eqview::handles;
using FT = teq::FilterType;

static int failures = 0;
static void check (bool ok, const char* what)
{
    if (! ok) { std::printf ("FAIL: %s\n", what); ++failures; }
    else        std::printf ("ok:   %s\n", what);
}
static bool nearEq (double a, double b, double tol) { return std::abs (a - b) <= tol; }

int main()
{
    //==========================================================================================
    // Type axioms: the classifiers must partition the 9 core types exactly as the editing model
    // requires — gain node only for bell/shelves/tilt; Q-whiskers for resonant types; slope-
    // whiskers for the variable-order cuts/notch/BP; whiskerRelevant = the union.
    {
        check (h::hasGain (FT::Bell) && h::hasGain (FT::LowShelf) && h::hasGain (FT::HighShelf) && h::hasGain (FT::Tilt),
               "hasGain covers bell + both shelves + tilt");
        check (! h::hasGain (FT::HighPass) && ! h::hasGain (FT::LowPass) && ! h::hasGain (FT::BandPass)
               && ! h::hasGain (FT::Notch) && ! h::hasGain (FT::AllPass),
               "hasGain excludes cuts / BP / notch / all-pass");
        check (h::isCut (FT::HighPass) && h::isCut (FT::LowPass), "isCut = HP + LP only");
        check (! h::isCut (FT::Bell) && ! h::isCut (FT::Notch), "isCut excludes everything else");
        check (h::slopeWhisker (FT::HighPass) && h::slopeWhisker (FT::LowPass)
               && h::slopeWhisker (FT::Notch) && h::slopeWhisker (FT::BandPass),
               "slopeWhisker = the variable-order family (HP/LP/Notch/BP)");
        check (! h::slopeWhisker (FT::Bell) && ! h::slopeWhisker (FT::LowShelf) && ! h::slopeWhisker (FT::AllPass),
               "slopeWhisker excludes Q-driven types");
        // whiskerRelevant is the union of qRelevant and isCut — every type that shows a handle
        for (FT t : { FT::Bell, FT::LowShelf, FT::HighShelf, FT::HighPass, FT::LowPass,
                      FT::BandPass, FT::Notch, FT::AllPass })
            check (h::whiskerRelevant (t), "whiskerRelevant covers every handled type");
        check (! h::whiskerRelevant (FT::Tilt), "Tilt has no whisker (gain-only, no width handle)");
        // The variable-order types Notch/BP are DELIBERATELY both Q-relevant (their -3 dB width,
        // edited in the strip) AND slope-driven (the whisker encodes the order). That is not an
        // ambiguity: whiskerBw resolves it deterministically — slopeWhisker WINS, so the whisker
        // always encodes slope for these, never Q. (This pins the design, not a wrong "disjoint"
        // assumption a naive theory might make.)
        check (h::qRelevant (FT::Notch) && h::slopeWhisker (FT::Notch), "Notch is BOTH Q- and slope-bearing (variable order)");
        check (h::qRelevant (FT::BandPass) && h::slopeWhisker (FT::BandPass), "BandPass is BOTH Q- and slope-bearing");
        // whiskerBw must pick the SLOPE path for a slope-whisker type (ignoring its Q) and the Q
        // path otherwise — a lone deterministic resolver, no ambiguity at the drag.
        check (nearEq (h::whiskerBw (FT::Notch, 5.0, 24), h::slopeBwForIndex (h::slopeIndexFromDb (24)), 1e-12),
               "whiskerBw for Notch uses SLOPE (Q ignored)");
        check (nearEq (h::whiskerBw (FT::Bell, 5.0, 24), h::whiskerBwForQ (5.0), 1e-12),
               "whiskerBw for Bell uses Q (slope ignored)");

        // [crew: opus 4a] restsOnZeroDb — the node-at-0-dB classifier the two draw paths must
        // share: exactly the gain-less surgical/phase types (cuts, notch, all-pass). It must be
        // disjoint from hasGain and, with hasGain, leave ONLY BandPass to ride the composite.
        check (h::restsOnZeroDb (FT::HighPass) && h::restsOnZeroDb (FT::LowPass)
               && h::restsOnZeroDb (FT::Notch) && h::restsOnZeroDb (FT::AllPass),
               "opus4a: restsOnZeroDb = cuts + notch + all-pass");
        check (! h::restsOnZeroDb (FT::Bell) && ! h::restsOnZeroDb (FT::BandPass) && ! h::restsOnZeroDb (FT::Tilt)
               && ! h::restsOnZeroDb (FT::LowShelf) && ! h::restsOnZeroDb (FT::HighShelf),
               "opus4a: restsOnZeroDb excludes gain types, shelves and BandPass");
        for (FT t : { FT::Bell, FT::LowShelf, FT::HighShelf, FT::HighPass, FT::LowPass,
                      FT::BandPass, FT::Notch, FT::AllPass, FT::Tilt })
        {
            check (! (h::hasGain (t) && h::restsOnZeroDb (t)), "opus4a: hasGain and restsOnZeroDb are disjoint");
            // exactly ONE of {hasGain, restsOnZeroDb, composite} holds — and 'composite' is only BandPass
            const bool composite = ! h::hasGain (t) && ! h::restsOnZeroDb (t);
            if (composite) check (t == FT::BandPass, "opus4a: only BandPass rides the composite");
        }
    }

    //==========================================================================================
    // Q <-> bandwidth calibration: the log-linear map's documented endpoints (Q=40 -> 0.2 oct,
    // Q=0.1 -> 0.833 oct), monotonic decreasing (wider band = lower Q), and a clean roundtrip in
    // the interior. Clamping outside [0.1, 40].
    {
        check (nearEq (h::whiskerBwForQ (40.0),  0.2,   1e-12), "Q=40 -> 0.2 oct half-bandwidth (max Q, narrow)");
        check (nearEq (h::whiskerBwForQ (0.1),   0.833, 1e-12), "Q=0.1 -> 0.833 oct (min Q, wide)");
        // the geometric-mean Q (log midpoint) lands at the bandwidth midpoint
        const double qMid = std::sqrt (0.1 * 40.0);
        check (nearEq (h::whiskerBwForQ (qMid), 0.5 * (0.2 + 0.833), 1e-9), "the log-mid Q maps to the bandwidth midpoint");
        // monotonic decreasing across the range
        bool mono = true; double prev = h::whiskerBwForQ (0.1);
        for (double q = 0.2; q <= 40.0; q *= 1.3) { const double bw = h::whiskerBwForQ (q); mono = mono && (bw < prev); prev = bw; }
        check (mono, "bandwidth strictly decreases as Q rises");
        // clamp: Q beyond the range saturates at the endpoints
        check (nearEq (h::whiskerBwForQ (1000.0), 0.2,   1e-12), "Q above 40 clamps to 0.2 oct");
        check (nearEq (h::whiskerBwForQ (0.001),  0.833, 1e-12), "Q below 0.1 clamps to 0.833 oct");
        // roundtrip Q -> bw -> Q inside the range
        for (double q : { 0.2, 0.5, 1.0, 2.0, 8.0, 25.0 })
            check (nearEq (h::whiskerQForBw (h::whiskerBwForQ (q)), q, q * 1e-9), "Q -> bw -> Q roundtrip is exact");
        // and the reverse roundtrip bw -> Q -> bw
        for (double bw : { 0.25, 0.4, 0.5, 0.7 })
            check (nearEq (h::whiskerBwForQ (h::whiskerQForBw (bw)), bw, 1e-9), "bw -> Q -> bw roundtrip is exact");
    }

    //==========================================================================================
    // Slope table: index<->dB, and the slope-index<->bandwidth map (steeper = narrower, i0 widest
    // at 0.833, i6 narrowest at 0.2), with roundtrips and out-of-table fallback.
    {
        const int db[7] = { 6, 12, 24, 36, 48, 72, 96 };
        for (int i = 0; i < 7; ++i) check (h::slopeIndexFromDb (db[i]) == i, "slopeIndexFromDb inverts the slope table");
        check (h::slopeIndexFromDb (18) == 1, "an off-table slope falls back to index 1 (12 dB/oct)");
        check (nearEq (h::slopeBwForIndex (0), 0.833, 1e-12), "slope index 0 = widest handle (0.833 oct)");
        check (nearEq (h::slopeBwForIndex (6), 0.2,   1e-12), "slope index 6 = narrowest handle (0.2 oct)");
        bool mono = true; double prev = h::slopeBwForIndex (0);
        for (int i = 1; i <= 6; ++i) { const double bw = h::slopeBwForIndex (i); mono = mono && (bw < prev); prev = bw; }
        check (mono, "steeper slope index gives a strictly narrower handle");
        for (int i = 0; i <= 6; ++i) check (h::slopeIndexForBw (h::slopeBwForIndex (i)) == i, "slope index -> bw -> index roundtrip");
        check (h::slopeIndexForBw (1.0) == 0 && h::slopeIndexForBw (0.0) == 6, "slope bw clamps to the end indices");
    }

    //==========================================================================================
    // Whisker geometry: the two endpoints are symmetric in LOG frequency about the node (equal
    // pixel offset on a log axis), sit at the node's own Y, and the right endpoint lands exactly
    // on freqToX(f0 * 2^bw).
    {
        eqview::PlotMap pm; pm.width = 1000.0f; pm.height = 400.0f; pm.plotBottom = 380.0f;
        pm.freqMin = 20.0; pm.freqMax = 28000.0;
        const double f0 = 1000.0, bw = 0.5;   // half a bandwidth up = 2^0.5 ~ 1.414x
        const h::Pt node { pm.freqToX (f0), 123.0f };
        const auto e = h::whiskerEndsPx (pm, node, f0, bw);
        check (nearEq (e.right.y, node.y, 1e-6) && nearEq (e.left.y, node.y, 1e-6), "whisker endpoints sit at the node's Y");
        check (nearEq (e.right.x, pm.freqToX (f0 * std::exp2 (bw)), 1e-3), "right endpoint = freqToX(f0 * 2^bw)");
        // symmetry in pixels (log-x): node.x - left == right - node.x
        check (nearEq (node.x - e.left.x, e.right.x - node.x, 1e-3), "endpoints are symmetric in pixels about the node");
        // that pixel symmetry means the LEFT endpoint is at f0 / 2^bw in frequency
        check (nearEq (pm.xToFreq (e.left.x), f0 * std::exp2 (-bw), f0 * 1e-3), "left endpoint = f0 / 2^bw (log-symmetric)");
        // zero bandwidth collapses both handles onto the node
        const auto z = h::whiskerEndsPx (pm, node, f0, 0.0);
        check (nearEq (z.left.x, node.x, 1e-4) && nearEq (z.right.x, node.x, 1e-4), "bw=0 collapses handles onto the node");

        // [crew: codex] EDGE MIRROR — the left endpoint is the PIXEL mirror of the right about the
        // node, NOT freqToX(f0/2^bw). At the high edge the right clamps to width while the left
        // mirrors the CLAMPED dx (so it does NOT reach f0/2^bw); at the low edge the left can go
        // NEGATIVE (off-screen) even though freqToX never returns < 0. Pin both.
        const h::Pt nHi { pm.freqToX (26000.0), 50.0f };
        const auto eHi = h::whiskerEndsPx (pm, nHi, 26000.0, 0.833);
        check (nearEq (eHi.right.x, pm.width, 1e-3), "high-edge whisker: right endpoint clamps to width");
        check (nearEq (eHi.left.x, 2.0f * nHi.x - eHi.right.x, 1e-4), "high-edge whisker: left is the clamped pixel-mirror, not f0/2^bw");
        check (eHi.left.x < nHi.x, "high-edge left stays left of the node (finite mirror, not re-clamped to the node)");
        const h::Pt nLo { pm.freqToX (25.0), 50.0f };   // f0=25, bw=0.5 -> left maps below freqMin
        const auto eLo = h::whiskerEndsPx (pm, nLo, 25.0, 0.5);
        check (eLo.left.x < 0.0f, "low-edge whisker: left endpoint goes NEGATIVE (pixel mirror, not a freq clamp)");
        check (nearEq (eLo.left.x, 2.0f * nLo.x - eLo.right.x, 1e-4), "low-edge whisker: left is the exact pixel-mirror of right");
    }

    //==========================================================================================
    // [crew: codex] whiskerBw resolution at the BandPass boundary (slope wins, mirroring Notch),
    // and slopeIndexForBw rounds half AWAY from zero at the exact index midpoint.
    {
        check (nearEq (h::whiskerBw (FT::BandPass, 40.0, 6), h::slopeBwForIndex (0), 1e-12),
               "whiskerBw for BandPass uses SLOPE (Q=40 ignored -> 0.833, not 0.2)");
        // Rounding to the nearest index: bw for t*6 = 2.4 rounds to 2, for 2.6 rounds to 3.
        // (The exact arithmetic midpoint is NOT a stable pin — it lands at t*6 = 2.4999…96 in float,
        // so std::round gives 2, not the "half-away 3" a naive derivation predicts. Pin the clear
        // sides instead of the knife-edge.)
        auto bwForT6 = [] (double t6) { return 0.833 - (t6 / 6.0) * (0.833 - 0.2); };
        check (h::slopeIndexForBw (bwForT6 (2.4)) == 2, "bw at t*6=2.4 rounds to index 2");
        check (h::slopeIndexForBw (bwForT6 (2.6)) == 3, "bw at t*6=2.6 rounds to index 3");
    }

    //==========================================================================================
    // [crew: deepseek] LOG-SHIFT INVARIANCE — the defining property of a log ruler: a bw-octave
    // whisker is the SAME pixel width anywhere on the axis (f0 and its 2^bw partner shift together).
    {
        eqview::PlotMap pm; pm.width = 1000.0f; pm.height = 400.0f; pm.plotBottom = 380.0f;
        pm.freqMin = 20.0; pm.freqMax = 28000.0;
        const double bw = 0.4;
        auto dxAt = [&] (double f0) { const h::Pt n { pm.freqToX (f0), 0.0f };
                                      return h::whiskerEndsPx (pm, n, f0, bw).right.x - n.x; };
        const double d1 = dxAt (100.0), d2 = dxAt (1000.0), d3 = dxAt (5000.0);
        check (nearEq (d1, d2, 1e-3) && nearEq (d2, d3, 1e-3), "deepseek: a bw-octave whisker is the same PIXELS at 100/1000/5000 Hz (log-shift invariant)");
    }

    //==========================================================================================
    // [crew: deepseek] whiskerQForBw is NOT the inverse of whiskerBwForQ OUTSIDE [0.1, 40]: an
    // out-of-range Q collapses to an endpoint bandwidth, and mapping back yields the ENDPOINT Q,
    // not the original — pin the non-invertibility (a naive "roundtrip always holds" is false).
    {
        check (nearEq (h::whiskerQForBw (h::whiskerBwForQ (200.0)), 40.0, 1e-9),
               "deepseek: Q=200 -> bw -> Q collapses to 40 (clamped, NOT invertible)");
        check (nearEq (h::whiskerQForBw (h::whiskerBwForQ (0.01)), 0.1, 1e-9),
               "deepseek: Q=0.01 -> bw -> Q collapses to 0.1 (clamped)");
    }

    //==========================================================================================
    // [crew: deepseek] Affine-in-log-Q identity: equal Q RATIOS produce equal bandwidth DELTAS
    // (the calibration is linear in log Q). A x2 Q step is a constant bw drop anywhere in-range.
    {
        const double step = h::whiskerBwForQ (1.0) - h::whiskerBwForQ (2.0);
        check (nearEq (h::whiskerBwForQ (2.0) - h::whiskerBwForQ (4.0),  step, 1e-9), "deepseek: each Q-doubling drops bw by a constant (2->4)");
        check (nearEq (h::whiskerBwForQ (5.0) - h::whiskerBwForQ (10.0), step, 1e-9), "deepseek: same constant bw drop at 5->10 (affine in log Q)");
        // slope table is exactly linear: equal index steps = equal bw steps
        const double sstep = h::slopeBwForIndex (0) - h::slopeBwForIndex (1);
        for (int i = 1; i < 6; ++i)
            check (nearEq (h::slopeBwForIndex (i) - h::slopeBwForIndex (i + 1), sstep, 1e-12), "deepseek: slope table is exactly linear in index");
        check (nearEq (sstep, (0.833 - 0.2) / 6.0, 1e-12), "deepseek: the slope step is exactly (0.833-0.2)/6");
    }

    //==========================================================================================
    // [crew: opus A] Log-linearity at a NON-endpoint, NON-midpoint (the quarter point) — the
    // existing endpoints+geomean checks pass for any monotone curve through those 3 points; this
    // pins the map is actually linear in log10(Q). Q = 40*(0.1/40)^0.25 -> t=0.25 -> bw=0.35825.
    {
        const double qQuarter = 40.0 * std::pow (0.1 / 40.0, 0.25);
        check (nearEq (h::whiskerBwForQ (qQuarter), 0.2 + 0.25 * (0.833 - 0.2), 1e-9),
               "opus A: whiskerBwForQ is linear in log10(Q) at the quarter point");
    }

    //==========================================================================================
    // [crew: opus B] whiskerQForBw clamps at the BANDWIDTH endpoints — the untested direction, and
    // the real drag contract (mouseDrag feeds unclamped bw straight in). A bw INSIDE 0.2 oct yields
    // MAX Q (the UX asymmetry: the display then snaps the handle back out to 0.2).
    {
        check (nearEq (h::whiskerQForBw (0.15), 40.0, 1e-12), "opus B: bw below 0.2 oct -> max Q 40 (clamped)");
        check (nearEq (h::whiskerQForBw (0.95), 0.1,  1e-12), "opus B: bw above 0.833 oct -> min Q 0.1 (clamped)");
    }

    //==========================================================================================
    // [crew: opus C] slopeIndexForBw half-step rounding is ties-AWAY-from-zero (std::round). The
    // index 0/1 midpoint gives t*6 = 0.5 EXACTLY (0.5 is representable) -> round(0.5) = 1.
    {
        const double mid01 = 0.5 * (h::slopeBwForIndex (0) + h::slopeBwForIndex (1));   // t*6 == 0.5 exactly
        check (h::slopeIndexForBw (mid01) == 1, "opus C: exact 0/1 midpoint rounds AWAY from zero to index 1");
        const double mid56 = 0.5 * (h::slopeBwForIndex (5) + h::slopeBwForIndex (6));
        check (h::slopeIndexForBw (mid56) == 6, "opus C: 5/6 midpoint rounds to index 6");
    }

    //==========================================================================================
    // [crew: opus D] The "left = f0/2^bw" invariant is FALSE near the freqToX clamp — the geometry
    // is PIXEL-symmetric always, FREQUENCY-symmetric only when neither endpoint saturates. Pins
    // the real contract so an "honest" rewrite (independent freqToX(f0/2^bw)) would be caught.
    {
        eqview::PlotMap pm; pm.width = 1000.0f; pm.height = 400.0f; pm.plotBottom = 380.0f;
        pm.freqMin = 20.0; pm.freqMax = 28000.0;
        const double f0 = 20000.0, bw = 1.0;   // f0*2 = 40000 clamps to 28000
        const h::Pt node { pm.freqToX (f0), 0.0f };
        const auto e = h::whiskerEndsPx (pm, node, f0, bw);
        check (nearEq (e.right.x, pm.width, 1e-3), "opus D: right endpoint saturates at width (40k clamps to 28k)");
        check (nearEq (node.x - e.left.x, e.right.x - node.x, 1e-4), "opus D: PIXEL symmetry still holds at the clamp");
        // frequency symmetry is BROKEN: xToFreq(left) is NOT f0/2^bw (10 kHz) — it's well above it
        check (pm.xToFreq (e.left.x) > 12000.0, "opus D: FREQUENCY symmetry breaks — left is NOT f0/2^bw at the clamp");
    }

    //==========================================================================================
    // Grab radius + distance helpers (the hit-test primitives).
    {
        check (nearEq (h::grabRadiusSq (6.0f, 5.0f), 121.0f, 1e-4), "grab radius squares (6+5)^2 = 121");
        check (nearEq (h::distanceSq ({ 0.0f, 0.0f }, { 3.0f, 4.0f }), 25.0f, 1e-4), "distanceSq is the squared euclidean (3,4,5)");
        // a point exactly on the grab boundary: dist^2 == r^2
        check (nearEq (h::distanceSq ({ 0.0f, 0.0f }, { 11.0f, 0.0f }), h::grabRadiusSq (6.0f, 5.0f), 1e-3),
               "a point at reach distance sits exactly on the grab boundary");
    }

    std::printf (failures == 0 ? "HandleMath: all checks passed\n" : "HandleMath: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
