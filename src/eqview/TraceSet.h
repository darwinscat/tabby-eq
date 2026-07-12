// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <teq/EqBand.h>
#include <teq/EqEngine.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>

//==============================================================================
// TraceSet — the response-curve calculator of the EQ view (eqview layer, step 0 incubation):
// owns the per-band parameter snapshot + the per-lane section designs, and evaluates dB curves
// for single lanes and for the per-axis composites. JUCE-free; the band/lane/axis model is core's
// (teq::) — the owner only supplies a reader for its parameter source and the sample rate.
//
// The curves are DESIGNED + evaluated at an oversampled "display" rate (>= 96k), not the real fs.
// A matched biquad's magnitude is even about fs/2, so at low real rates (44.1/48k) an LP/BP would
// visibly FLATTEN as it nears Nyquist (slope -> 0) and then kink; oversampling pushes that
// flattening far past the visible axis so the curve shows the smooth analog INTENT
// (FabFilter-style) and looks identical at every rate. (An ANALYZER must keep using the real fs —
// that is true measured content, not a design.)
namespace eqview
{

struct TraceSet
{
    static constexpr int maxBands = teq::EqEngine::kMaxBands;
    static constexpr int numLanes = teq::kNumLanes;

    double sampleRate() const noexcept { return fs; }
    double designFs()   const noexcept { return std::max (fs, 96000.0); }

    // Snapshot every band through the owner's reader and (re)design every ENABLED lane.
    // Disabled lanes are left as a default BandDesign — which is an IDENTITY design (core defaults
    // one identity biquad, n = 1), so an un-designed lane evaluates to exactly 0 dB by contract.
    template <class ReadBand>   // teq::BandParams readBand (int b)
    void refresh (double sampleRateHz, ReadBand&& readBand)
    {
        fs = sampleRateHz > 0.0 ? sampleRateHz : 44100.0;
        const double dfs = designFs();
        for (int b = 0; b < maxBands; ++b)
        {
            bands[(size_t) b] = readBand (b);
            for (int L = 0; L < numLanes; ++L)
                designs[(size_t) b][(size_t) L] = (bands[(size_t) b].on && bands[(size_t) b].lanes[(size_t) L].on)
                                    ? teq::designBand (teq::laneView (bands[(size_t) b], (teq::Lane) L), dfs)
                                    : teq::BandDesign {};
        }
    }

    const teq::BandParams& param  (int b) const noexcept
    {
        assert (b >= 0 && b < maxBands);
        return bands[(size_t) b];
    }
    const teq::BandDesign& design (int b, int lane) const noexcept
    {
        assert (b >= 0 && b < maxBands && lane >= 0 && lane < numLanes);
        return designs[(size_t) b][(size_t) lane];
    }

    // One lane's response (dB) at frequency f — oversampled so LP/BP dive smoothly, no Nyquist
    // flatten; above 0.499·designFs the evaluation freezes at the clamp (display-only tail).
    double bandDb (int b, double f, int lane) const noexcept
    {
        assert (b >= 0 && b < maxBands && lane >= 0 && lane < numLanes);
        const auto&  d   = designs[(size_t) b][(size_t) lane];
        const double dfs = designFs();
        const double w   = 2.0 * kPi * std::min (f, 0.499 * dfs) / dfs;
        std::complex<double> h { 1.0, 0.0 };
        for (int s = 0; s < d.n; ++s) h *= teq::evalCoeffs (d.sec[s], w);
        return 20.0 * std::log10 (std::max (1.0e-9, std::abs (h)));
    }

    // One stereo axis's composite (dB): every band's ST lane folds into EVERY axis, plus the
    // axis's own-domain lane where enabled (teq's normative lane/axis model).
    double compositeDbAxis (double f, teq::Axis a) const noexcept
    {
        const double dfs = designFs();
        const double w   = 2.0 * kPi * std::min (f, 0.499 * dfs) / dfs;
        const teq::Lane axl = teq::axisLane (a);
        const int axi = (int) axl;
        std::complex<double> h { 1.0, 0.0 };
        for (int b = 0; b < maxBands; ++b)
        {
            if (! bands[(size_t) b].on) continue;
            if (teq::laneActive (bands[(size_t) b], teq::Lane::Stereo))
                for (int s = 0; s < designs[(size_t) b][0].n; ++s) h *= teq::evalCoeffs (designs[(size_t) b][0].sec[s], w);
            if (a != teq::Axis::Stereo && teq::laneActive (bands[(size_t) b], axl))
                for (int s = 0; s < designs[(size_t) b][(size_t) axi].n; ++s) h *= teq::evalCoeffs (designs[(size_t) b][(size_t) axi].sec[s], w);
        }
        return 20.0 * std::log10 (std::max (1.0e-9, std::abs (h)));
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    double fs = 44100.0;
    teq::BandParams bands[maxBands];
    teq::BandDesign designs[maxBands][numLanes];
};

} // namespace eqview
