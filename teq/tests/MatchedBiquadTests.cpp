// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

// Proves the matched (Vicanek) biquad family does its job: resonant filters and shelves
// pushed near Nyquist stay true to their analog prototype where the RBJ cookbook cramps.
// This is the correctness gate behind decision #3 (matched coeffs from day one).

#include "TestUtil.h"

#include <teq/MatchedBiquad.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace teq;

namespace
{
    // Analog resonant-lowpass prototype magnitude at real frequency f (Hz). |H(i w0)| = Q.
    double analogLowpassMag (double f, double f0, double Q) noexcept
    {
        const double w0 = 2.0 * kPi * f0, w = 2.0 * kPi * f;
        const double re = w0 * w0 - w * w, im = w * w0 / Q;
        return (w0 * w0) / std::sqrt (re * re + im * im);
    }

    // Analog 2-pole Butterworth high-shelf magnitude (Vicanek 2poleShelvingFits eq. 1),
    // gain G at high frequencies, unity at DC. sg = G^(1/2) = (G^(1/4))^2.
    double analogHighShelfMag (double f, double f0, double G) noexcept
    {
        const double sg = std::sqrt (G), r2 = (f / f0) * (f / f0);
        const double numSq = (1.0 - sg * r2) * (1.0 - sg * r2) + 2.0 * sg * r2;
        const double denSq = (1.0 - r2 / sg) * (1.0 - r2 / sg) + 2.0 * r2 / sg;
        return std::sqrt (numSq / denSq);
    }

    double db (double x) noexcept { return 20.0 * std::log10 (std::max (1e-12, x)); }
}

void runMatchedBiquadTests()
{
    using namespace teqtest;
    const double fs = 44100.0;
    auto w = [fs] (double f) { return 2.0 * kPi * f / fs; };

    group ("peaking: 0 dB is identity (flat) everywhere");
    {
        const auto c = matched::peakingDb (3000.0, fs, 2.0, 0.0);
        for (double f : { 50.0, 500.0, 3000.0, 9000.0, 18000.0 })
            expectNear (c.magnitudeDb (w (f)), 0.0, 1e-6, "flat at " + std::to_string ((int) f));
    }

    group ("peaking: nails the centre gain mid-band AND in the air band");
    {
        const auto mid = matched::peakingDb (1000.0, fs, 4.0, 6.0);
        expectNear (mid.magnitudeDb (w (1000.0)), 6.0, 0.02, "+6 dB @ 1k");
        const auto hi = matched::peakingDb (16000.0, fs, 4.0, 6.0);
        expectNear (hi.magnitudeDb (w (16000.0)), 6.0, 0.05, "+6 dB @ 16k air bell");
        expectTrue (hi.isStable(), "air bell stable");
    }

    group ("lowpass: matched hits analog |H(w0)| = Q where RBJ cramps");
    {
        const double f0 = 0.40 * fs, Q = 4.0;
        const double m = matched::lowpass (f0, fs, Q).magnitude (w (f0));
        const double r = rbj::lowpass     (f0, fs, Q).magnitude (w (f0));
        std::printf ("      |H(w0)|: Q=%.1f  matched=%.4f  rbj=%.4f\n", Q, m, r);
        expectNear (m, Q, 1e-3, "matched == Q at cutoff (by construction)");
        expectTrue (std::fabs (m - Q) < std::fabs (r - Q), "matched closer to analog than RBJ");
    }

    group ("lowpass: matched tracks the analog curve better than RBJ across the band");
    {
        const double f0 = 0.32 * fs, Q = 3.0;
        const auto m = matched::lowpass (f0, fs, Q);
        const auto r = rbj::lowpass     (f0, fs, Q);
        double mMax = 0.0, rMax = 0.0;
        for (double frac = 0.02; frac <= 0.47; frac += 0.005)
        {
            const double ww = 2.0 * kPi * frac, ref = analogLowpassMag (frac * fs, f0, Q);
            mMax = std::max (mMax, std::fabs (m.magnitude (ww) - ref));
            rMax = std::max (rMax, std::fabs (r.magnitude (ww) - ref));
        }
        std::printf ("      max |digital-analog|: matched=%.4f  rbj=%.4f\n", mMax, rMax);
        expectTrue (mMax < rMax, "matched max error < RBJ max error");
    }

    group ("high shelf: DC = 0 dB, centre = +G/2, Nyquist = +G");
    {
        const double f0 = 2000.0, gDb = 12.0;
        const auto c = matched::highShelfDb (f0, fs, gDb);
        expectNear (c.magnitudeDb (0.0),     0.0,  0.05, "unity at DC");
        expectNear (c.magnitudeDb (w (f0)),  6.0,  0.6,  "+6 dB at the corner");
        expectNear (c.magnitudeDb (kPi),    12.0,  0.3,  "+12 dB plateau at Nyquist");
        expectTrue (c.isStable(), "high shelf stable");
    }

    group ("low shelf: DC = +G, centre = +G/2, top = 0 dB");
    {
        const double f0 = 200.0, gDb = 12.0;
        const auto c = matched::lowShelfDb (f0, fs, gDb);
        expectNear (c.magnitudeDb (0.0),    12.0,  0.3,  "+12 dB at DC");
        expectNear (c.magnitudeDb (w (f0)),  6.0,  0.6,  "+6 dB at the corner");
        expectNear (c.magnitudeDb (kPi),     0.0,  0.3,  "unity at the top");
        expectTrue (c.isStable(), "low shelf stable");
    }

    group ("high shelf: matched tracks analog better than RBJ near Nyquist");
    {
        const double f0 = 10000.0, gDb = 15.0, G = std::pow (10.0, gDb / 20.0);
        const auto m = matched::highShelfDb (f0, fs, gDb);
        const auto r = rbj::highShelf      (f0, fs, G);
        double mMax = 0.0, rMax = 0.0;
        for (double frac = 0.02; frac <= 0.47; frac += 0.005)
        {
            const double ww = 2.0 * kPi * frac, refDb = db (analogHighShelfMag (frac * fs, f0, G));
            mMax = std::max (mMax, std::fabs (m.magnitudeDb (ww) - refDb));
            rMax = std::max (rMax, std::fabs (r.magnitudeDb (ww) - refDb));
        }
        std::printf ("      max |digital-analog| dB: matched=%.3f  rbj=%.3f\n", mMax, rMax);
        expectTrue (mMax < rMax, "matched max error (dB) < RBJ max error (dB)");
    }

    group ("stability across a freq / Q / gain grid");
    {
        for (double f0 : { 40.0, 200.0, 1000.0, 8000.0, 18000.0, 21000.0 })
            for (double Q : { 0.3, 0.707, 2.0, 8.0 })
            {
                const std::string at = " (f0=" + std::to_string ((int) f0) + ", Q=" + std::to_string (Q) + ")";
                expectTrue (matched::peakingDb (f0, fs, Q, 6.0).isStable(), "peaking unstable" + at);
                expectTrue (matched::lowpass   (f0, fs, Q).isStable(),      "lowpass unstable" + at);
                expectTrue (matched::highpass  (f0, fs, Q).isStable(),      "highpass unstable" + at);
                expectTrue (matched::bandpass  (f0, fs, Q).isStable(),      "bandpass unstable" + at);
            }
        for (double f0 : { 60.0, 500.0, 3000.0, 12000.0, 18000.0 })
            for (double gDb : { -12.0, -3.0, 3.0, 12.0 })
            {
                const std::string at = " (f0=" + std::to_string ((int) f0) + ", g=" + std::to_string (gDb) + ")";
                expectTrue (matched::highShelfDb (f0, fs, gDb).isStable(), "high shelf unstable" + at);
                expectTrue (matched::lowShelfDb  (f0, fs, gDb).isStable(), "low shelf unstable" + at);
            }
    }
}
