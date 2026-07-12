// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.
//
// JUCE-free theory unit for the eqview TraceSet (src/eqview/TraceSet.h) — the response-curve
// calculator. House policy: expectations come from THEORY, not from the code — the matched-design
// contract (|H| exact at the band centre), log-multiplicativity of cascades, the teq lane/axis
// folding axioms, the analytic Butterworth magnitude, and the documented display-rate rules.

#include "eqview/TraceSet.h"

#include <cmath>
#include <cstdio>

static int failures = 0;

static void check (bool ok, const char* what)
{
    if (! ok) { std::printf ("FAIL: %s\n", what); ++failures; }
    else        std::printf ("ok:   %s\n", what);
}

static bool nearEq (double a, double b, double tol) { return std::abs (a - b) <= tol; }

static teq::BandParams bell (double freq, double gainDb, double q, teq::Lane lane = teq::Lane::Stereo)
{
    teq::BandParams bp;
    bp.on = true; bp.type = teq::FilterType::Bell;
    for (int L = 0; L < teq::kNumLanes; ++L) bp.lanes[(size_t) L].on = false;
    auto& ln = bp.lanes[(size_t) lane];
    ln.on = true; ln.freq = freq; ln.gainDb = gainDb; ln.Q = q; ln.slope = 12;
    return bp;
}

int main()
{
    using TS = eqview::TraceSet;

    //==========================================================================================
    // Display-rate rules: the design rate floors at 96k (the anti-Nyquist-flatten rule) and the
    // sample-rate guard replaces a dead host rate with 44.1k.
    {
        TS ts;
        ts.refresh (44100.0,  [] (int) { return teq::BandParams {}; });
        check (nearEq (ts.designFs(), 96000.0, 1e-9), "designFs floors at 96k for 44.1k hosts");
        ts.refresh (192000.0, [] (int) { return teq::BandParams {}; });
        check (nearEq (ts.designFs(), 192000.0, 1e-9), "designFs follows fs above the floor");
        ts.refresh (0.0,      [] (int) { return teq::BandParams {}; });
        check (nearEq (ts.sampleRate(), 44100.0, 1e-9), "a dead host rate falls back to 44.1k");
    }

    //==========================================================================================
    // Matched-design contract: a bell's response is EXACT at its centre (|H(fc)| = G — the whole
    // point of matched coefficients), and an OFF band / disabled lane contributes exactly 0 dB.
    {
        TS ts;
        ts.refresh (48000.0, [] (int b) { return b == 0 ? bell (1000.0, 6.0, 1.0) : teq::BandParams {}; });
        check (nearEq (ts.bandDb (0, 1000.0, 0), 6.0, 5e-3), "matched bell reads EXACTLY its gain at fc");
        check (nearEq (ts.bandDb (1, 1000.0, 0), 0.0, 1e-12), "an OFF band is exactly 0 dB");
        check (nearEq (ts.bandDb (0, 1000.0, 1), 0.0, 1e-12), "a disabled lane is exactly 0 dB");
        // far below/above the bell the response returns to unity (matched exact at DC)
        check (nearEq (ts.bandDb (0, 10.0, 0), 0.0, 0.05), "bell returns to 0 dB far below fc");
    }

    //==========================================================================================
    // Log-multiplicativity: a cascade's dB response is the SUM of its stages' dB responses —
    // the composite must equal the sum of the individual band curves at every frequency.
    {
        TS ts;
        ts.refresh (48000.0, [] (int b)
        {
            if (b == 0) return bell (200.0,  5.0, 0.8);
            if (b == 1) return bell (2500.0, -7.5, 2.0);
            return teq::BandParams {};
        });
        for (double f : { 50.0, 200.0, 700.0, 2500.0, 9000.0, 20000.0 })
        {
            char label[80]; std::snprintf (label, sizeof (label), "composite == sum of band curves at %g Hz", f);
            check (nearEq (ts.compositeDbAxis (f, teq::Axis::Mid),
                           ts.bandDb (0, f, 0) + ts.bandDb (1, f, 0), 1e-6), label);
        }
    }

    //==========================================================================================
    // Axis folding axioms (teq's normative lane/axis model): the ST lane folds into EVERY axis;
    // a domain lane folds ONLY into its own axis; the Stereo axis carries ST alone.
    {
        TS ts;
        ts.refresh (48000.0, [] (int b)
        {
            if (b == 0) return bell (1000.0, 6.0, 1.0, teq::Lane::Stereo);
            if (b == 1) return bell (1000.0, 3.0, 1.0, teq::Lane::Left);
            return teq::BandParams {};
        });
        check (nearEq (ts.compositeDbAxis (1000.0, teq::Axis::Left),   9.0, 5e-3), "Left axis = ST + Left lane (6+3)");
        check (nearEq (ts.compositeDbAxis (1000.0, teq::Axis::Right),  6.0, 5e-3), "Right axis = ST only");
        check (nearEq (ts.compositeDbAxis (1000.0, teq::Axis::Mid),    6.0, 5e-3), "Mid axis = ST only");
        check (nearEq (ts.compositeDbAxis (1000.0, teq::Axis::Side),   6.0, 5e-3), "Side axis = ST only");
        check (nearEq (ts.compositeDbAxis (1000.0, teq::Axis::Stereo), 6.0, 5e-3), "Stereo axis carries ST alone");
    }

    //==========================================================================================
    // The anti-flatten rule, refuted analytically: a 24 dB/oct LP at 10 kHz on a 44.1k host must
    // follow the ANALOG 4th-order Butterworth magnitude |H| = 1/sqrt(1+(f/fc)^8) at 20 kHz
    // (-24.07 dB) — a real-fs design would flatten toward the 22.05k mirror and miss high.
    {
        TS ts;
        ts.refresh (44100.0, [] (int b)
        {
            if (b != 0) return teq::BandParams {};
            teq::BandParams bp; bp.on = true; bp.type = teq::FilterType::LowPass;
            for (int L = 0; L < teq::kNumLanes; ++L) bp.lanes[(size_t) L].on = false;
            auto& ln = bp.lanes[0]; ln.on = true; ln.freq = 10000.0; ln.Q = 0.707; ln.slope = 24;
            return bp;
        });
        const double analytic20k = -10.0 * std::log10 (1.0 + std::pow (2.0, 8.0));   // 2 octaves? no: f/fc = 2 -> (2)^8
        check (nearEq (ts.bandDb (0, 20000.0, 0), analytic20k, 0.6),
               "24 dB/oct LP tracks the ANALOG Butterworth at 20 kHz on a 44.1k host (no Nyquist flatten)");
        check (ts.bandDb (0, 28000.0, 0) < ts.bandDb (0, 20000.0, 0) - 8.0,
               "the LP keeps diving past the audible top (the flatten would stall it)");
    }

    //==========================================================================================
    // Above 0.499·designFs the evaluation freezes at the clamp — identical values, not garbage.
    {
        TS ts;
        ts.refresh (48000.0, [] (int b) { return b == 0 ? bell (1000.0, 6.0, 1.0) : teq::BandParams {}; });
        const double fClamp = 0.499 * ts.designFs();
        check (nearEq (ts.bandDb (0, fClamp * 2.0, 0), ts.bandDb (0, fClamp, 0), 1e-12),
               "evaluation freezes exactly at the 0.499*designFs clamp");
        check (nearEq (ts.bandDb (0, 1.0e6, 0), ts.bandDb (0, fClamp, 0), 1e-12),
               "absurd frequencies read the frozen clamp value");
    }

    //==========================================================================================
    // A notch's null is bottomless in theory — the display floors it at exactly 20*log10(1e-9).
    {
        TS ts;
        ts.refresh (48000.0, [] (int b)
        {
            if (b != 0) return teq::BandParams {};
            teq::BandParams bp; bp.on = true; bp.type = teq::FilterType::Notch;
            for (int L = 0; L < teq::kNumLanes; ++L) bp.lanes[(size_t) L].on = false;
            auto& ln = bp.lanes[0]; ln.on = true; ln.freq = 1000.0; ln.Q = 1.0; ln.slope = 12;
            return bp;
        });
        check (nearEq (ts.bandDb (0, 1000.0, 0), -180.0, 1e-3), "the notch null floors at exactly -180 dB (1e-9)");
        check (nearEq (ts.bandDb (0, 100.0, 0), 0.0, 0.1), "the notch passband stays at unity");
    }

    //==========================================================================================
    // [crew: codex] The -180 floor BREAKS additivity by design: two notches at the same fc each
    // floor individually (-180 + -180 = -360 as a sum), but the composite floors ONCE after the
    // product — the display must show -180, not -360.
    {
        TS ts;
        ts.refresh (48000.0, [] (int b)
        {
            if (b > 1) return teq::BandParams {};
            teq::BandParams bp; bp.on = true; bp.type = teq::FilterType::Notch;
            for (int L = 0; L < teq::kNumLanes; ++L) bp.lanes[(size_t) L].on = false;
            auto& ln = bp.lanes[0]; ln.on = true; ln.freq = 1000.0; ln.Q = 1.0; ln.slope = 12;
            return bp;
        });
        check (nearEq (ts.bandDb (0, 1000.0, 0), -180.0, 1e-3), "codex: each notch floors at -180 individually");
        check (nearEq (ts.compositeDbAxis (1000.0, teq::Axis::Mid), -180.0, 1e-3),
               "codex: stacked notches floor ONCE in the composite (-180, not -360)");
    }

    //==========================================================================================
    // [crew: codex] Bypass contributes IDENTITY to the composite while the lane curve stays
    // readable — the ghost-curve contract: laneActive excludes bypassed lanes from the sum, but
    // the design still exists for the dimmed ghost drawing.
    {
        TS ts;
        ts.refresh (48000.0, [] (int b)
        {
            if (b == 0) return bell (1000.0, 6.0, 1.0);
            if (b == 1) { auto bp = bell (1000.0, 12.0, 1.0); bp.lanes[0].bypass = true; return bp; }
            return teq::BandParams {};
        });
        check (nearEq (ts.compositeDbAxis (1000.0, teq::Axis::Mid), 6.0, 5e-3),
               "codex: a bypassed lane folds as IDENTITY into the composite");
        check (nearEq (ts.bandDb (1, 1000.0, 0), 12.0, 5e-3),
               "codex: the bypassed lane's own curve stays readable (ghost contract)");
    }

    //==========================================================================================
    // [crew: codex] Sub-96k host rates are pixel-IDENTICAL: 44.1k and 48k both design at the 96k
    // floor, so every curve value must match exactly.
    {
        TS a, bset;
        auto reader = [] (int b) { return b == 0 ? bell (1000.0, 6.0, 1.0) : teq::BandParams {}; };
        a.refresh (44100.0, reader); bset.refresh (48000.0, reader);
        bool same = true;
        for (double f : { 20.0, 100.0, 1000.0, 5000.0, 20000.0 })
            same = same && nearEq (a.bandDb (0, f, 0), bset.bandDb (0, f, 0), 1e-12);
        check (same, "codex: 44.1k and 48k hosts draw bit-identical curves (both design at 96k)");
    }

    //==========================================================================================
    // [crew: codex] HP mirror of the anti-flatten pin: a 24 dB/oct HP at 1 kHz reads the ANALOG
    // 4th-order Butterworth |H| = r^N / sqrt(1 + r^2N) at r = 1/2 -> -24.098 dB.
    {
        TS ts;
        ts.refresh (44100.0, [] (int b)
        {
            if (b != 0) return teq::BandParams {};
            teq::BandParams bp; bp.on = true; bp.type = teq::FilterType::HighPass;
            for (int L = 0; L < teq::kNumLanes; ++L) bp.lanes[(size_t) L].on = false;
            auto& ln = bp.lanes[0]; ln.on = true; ln.freq = 1000.0; ln.Q = 0.707; ln.slope = 24;
            return bp;
        });
        const double r = 0.5;
        const double analytic = 20.0 * std::log10 (std::pow (r, 4.0) / std::sqrt (1.0 + std::pow (r, 8.0)));
        check (nearEq (ts.bandDb (0, 500.0, 0), analytic, 0.6), "codex: 24 dB/oct HP tracks analog Butterworth at fc/2");
    }

    //==========================================================================================
    // [crew: deepseek] An all-pass is PHASE-ONLY: |H| == 1 at every frequency; any magnitude
    // ripple is a coefficient-scaling defect.
    {
        TS ts;
        ts.refresh (48000.0, [] (int b)
        {
            if (b != 0) return teq::BandParams {};
            teq::BandParams bp; bp.on = true; bp.type = teq::FilterType::AllPass;
            for (int L = 0; L < teq::kNumLanes; ++L) bp.lanes[(size_t) L].on = false;
            auto& ln = bp.lanes[0]; ln.on = true; ln.freq = 1000.0; ln.Q = 1.0; ln.slope = 12;
            return bp;
        });
        bool flat = true;
        for (double f = 20.0; f < 40000.0; f *= 1.7) flat = flat && nearEq (ts.bandDb (0, f, 0), 0.0, 1e-6);
        check (flat, "deepseek: all-pass magnitude is identically 0 dB (phase-only, fp-rounding only)");
    }

    //==========================================================================================
    // [crew: deepseek] Tilt geometry, convention-agnostic: 0 dB at its pivot, endpoints
    // antisymmetric, and the total span across the axis equals the gain parameter.
    {
        TS ts;
        ts.refresh (48000.0, [] (int b)
        {
            if (b != 0) return teq::BandParams {};
            teq::BandParams bp; bp.on = true; bp.type = teq::FilterType::Tilt;
            for (int L = 0; L < teq::kNumLanes; ++L) bp.lanes[(size_t) L].on = false;
            auto& ln = bp.lanes[0]; ln.on = true; ln.freq = 1000.0; ln.Q = 0.707; ln.gainDb = 6.0; ln.slope = 12;
            return bp;
        });
        // Core's DOCUMENTED tilt contract (EqBand.h): lowShelfDb(-g) + highShelfDb(+g) — the gain
        // parameter is EACH side's plateau, so DC reads -g, the top reads +g, span = 2g.
        const double lo = ts.bandDb (0, 0.5, 0), hi = ts.bandDb (0, 0.499 * ts.designFs(), 0);
        check (nearEq (ts.bandDb (0, 1000.0, 0), 0.0, 0.1), "deepseek: tilt reads 0 dB at its pivot");
        check (nearEq (lo, -hi, 0.1), "deepseek: tilt endpoints are antisymmetric");
        check (nearEq (lo, -6.0, 0.15) && nearEq (hi, +6.0, 0.15),
               "deepseek/core-contract: tilt plateaus are FULL -g/+g (lowShelf(-g)+highShelf(+g))");
    }

    //==========================================================================================
    // [crew: deepseek] The family's symmetric-cut guarantee: bell(+G) * bell(-G) at the same
    // fc/Q is an exact reciprocal pair — the cascade must be 0 dB at EVERY frequency.
    {
        TS ts;
        ts.refresh (48000.0, [] (int b)
        {
            if (b == 0) return bell (2000.0, +8.0, 4.0);
            if (b == 1) return bell (2000.0, -8.0, 4.0);
            return teq::BandParams {};
        });
        bool flat = true;
        for (double f = 20.0; f < 40000.0; f *= 1.5) flat = flat && nearEq (ts.compositeDbAxis (f, teq::Axis::Mid), 0.0, 2e-3);
        check (flat, "deepseek: bell(+G) x bell(-G) cancels EXACTLY everywhere (symmetric-cut contract)");
    }

    //==========================================================================================
    // [crew: deepseek] Matched shelf plateaus are boundary conditions of the design: a +9 dB
    // high shelf reads 0 dB at DC and +9 at the top; a -7.5 dB low shelf reads -7.5 / 0.
    {
        auto shelfAt = [] (teq::FilterType t, double fc, double g)
        {
            TS ts;
            ts.refresh (48000.0, [&] (int b)
            {
                if (b != 0) return teq::BandParams {};
                teq::BandParams bp; bp.on = true; bp.type = t;
                for (int L = 0; L < teq::kNumLanes; ++L) bp.lanes[(size_t) L].on = false;
                auto& ln = bp.lanes[0]; ln.on = true; ln.freq = fc; ln.Q = 0.707; ln.gainDb = g; ln.slope = 12;
                return bp;
            });
            return std::pair<double, double> { ts.bandDb (0, 0.5, 0), ts.bandDb (0, 0.499 * ts.designFs(), 0) };
        };
        const auto hs = shelfAt (teq::FilterType::HighShelf, 8000.0, +9.0);
        check (nearEq (hs.first, 0.0, 0.1) && nearEq (hs.second, +9.0, 0.1), "deepseek: high-shelf plateaus 0 dB @DC, +9 @top (matched boundary)");
        const auto lsh = shelfAt (teq::FilterType::LowShelf, 200.0, -7.5);
        check (nearEq (lsh.first, -7.5, 0.1) && nearEq (lsh.second, 0.0, 0.1), "deepseek: low-shelf plateaus -7.5 @DC, 0 @top (matched boundary)");
    }

    //==========================================================================================
    // [crew: deepseek] Butterworth maximal flatness: a 48 dB/oct LP has NO positive passband
    // ripple, reads exactly -3.01 dB at fc (order-invariant), and declines monotonically after.
    {
        TS ts;
        ts.refresh (192000.0, [] (int b)
        {
            if (b != 0) return teq::BandParams {};
            teq::BandParams bp; bp.on = true; bp.type = teq::FilterType::LowPass;
            for (int L = 0; L < teq::kNumLanes; ++L) bp.lanes[(size_t) L].on = false;
            auto& ln = bp.lanes[0]; ln.on = true; ln.freq = 10000.0; ln.Q = 0.707; ln.slope = 48;
            return bp;
        });
        bool noRipple = true;
        for (double f = 100.0; f < 8000.0; f *= 1.4) noRipple = noRipple && (ts.bandDb (0, f, 0) <= 0.01);
        check (noRipple, "deepseek: Butterworth LP passband has no positive ripple (maximally flat)");
        check (nearEq (ts.bandDb (0, 10000.0, 0), -3.0103, 0.05), "deepseek: Butterworth LP reads -3.01 dB at fc (order-invariant)");
        bool monotone = true; double prev = ts.bandDb (0, 11000.0, 0);
        for (double f = 13000.0; f < 90000.0; f *= 1.3) { const double v = ts.bandDb (0, f, 0); monotone = monotone && (v < prev); prev = v; }
        check (monotone, "deepseek: Butterworth LP declines monotonically in the stopband");
    }

    //==========================================================================================
    // [crew: opus] POINT-bypass (the whole point, not a lane) removes the band from the composite
    // while its own curve stays designed — and the two bypass LEVELS gate independently.
    {
        TS ts;
        ts.refresh (48000.0, [] (int b)
        {
            if (b != 0) return teq::BandParams {};
            auto bp = bell (1000.0, 6.0, 1.0); bp.bypass = true; return bp;
        });
        check (nearEq (ts.compositeDbAxis (1000.0, teq::Axis::Mid), 0.0, 1e-9),
               "opus: point-bypass folds as identity into the composite");
        check (nearEq (ts.bandDb (0, 1000.0, 0), 6.0, 5e-3),
               "opus: the point-bypassed band's own curve stays designed (ghost)");
    }

    //==========================================================================================
    // [crew: opus] Domain-lane folding for Mid and Side — the axi=3/4 design indices, which the
    // Left-lane test cannot distinguish from "correctly ST-only".
    {
        TS ts;
        ts.refresh (48000.0, [] (int b)
        {
            if (b == 0) return bell (1000.0, 4.0, 1.0, teq::Lane::Mid);
            if (b == 1) return bell (1000.0, 2.0, 1.0, teq::Lane::Side);
            return teq::BandParams {};
        });
        check (nearEq (ts.compositeDbAxis (1000.0, teq::Axis::Mid),  4.0, 5e-3), "opus: Mid axis folds the Mid lane (axi=3)");
        check (nearEq (ts.compositeDbAxis (1000.0, teq::Axis::Side), 2.0, 5e-3), "opus: Side axis folds the Side lane (axi=4)");
        check (nearEq (ts.compositeDbAxis (1000.0, teq::Axis::Left), 0.0, 1e-9), "opus: Left axis sees neither M nor S lane");
        check (ts.design (0, (int) teq::Lane::Mid).n >= 1, "opus: design() exposes the designed lane sections");
    }

    std::printf (failures == 0 ? "TraceSet: all checks passed\n" : "TraceSet: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
