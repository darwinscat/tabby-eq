// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <teq/EqTypes.h>
#include <teq/Math.h>
#include <teq/MatchedBiquad.h>
#include <teq/Smoother.h>
#include <teq/Svf.h>

#include <algorithm>
#include <complex>

namespace teq
{

//==============================================================================
// Stateless band design + response — used by EqBand for audio AND by the GUI for a race-free
// curve (the GUI computes from a BandParams snapshot it owns, never reading engine internals).
struct BandDesign { BiquadCoeffs c0, c1; bool twoStage = false; };

inline BandDesign designBand (const BandParams& in, double fs) noexcept
{
    auto finiteOr = [] (double x, double fb) noexcept { return std::isfinite (x) ? x : fb; };
    BandParams p = in;
    p.freq   = std::clamp (finiteOr (p.freq, 1000.0), 10.0, 0.49 * fs);
    p.Q      = std::clamp (finiteOr (p.Q, 1.0), 0.05, 40.0);
    p.gainDb = std::clamp (finiteOr (p.gainDb, 0.0), -30.0, 30.0);

    BandDesign d;
    const bool cutTwoStage = (! p.swept) && (p.slope >= 24) && (p.type == FilterType::HighPass || p.type == FilterType::LowPass);
    d.twoStage = cutTwoStage || (p.type == FilterType::Tilt);

    if (cutTwoStage)
    {
        constexpr double q1 = 0.54119610, q2 = 1.30656296;   // 4th-order Butterworth section Qs
        if (p.type == FilterType::HighPass) { d.c0 = matched::highpass (p.freq, fs, q1); d.c1 = matched::highpass (p.freq, fs, q2); }
        else                                { d.c0 = matched::lowpass  (p.freq, fs, q1); d.c1 = matched::lowpass  (p.freq, fs, q2); }
    }
    else if (p.type == FilterType::Tilt)
    {
        d.c0 = matched::lowShelfDb  (p.freq, fs, -p.gainDb);   // lows down
        d.c1 = matched::highShelfDb (p.freq, fs,  p.gainDb);   // highs up  -> spectral tilt about f0
    }
    else
    {
        switch (p.type)
        {
            case FilterType::Bell:      d.c0 = matched::peakingDb  (p.freq, fs, p.Q, p.gainDb); break;
            case FilterType::LowShelf:  d.c0 = matched::lowShelfDb  (p.freq, fs, p.gainDb);     break;
            case FilterType::HighShelf: d.c0 = matched::highShelfDb (p.freq, fs, p.gainDb);     break;
            case FilterType::HighPass:  d.c0 = matched::highpass     (p.freq, fs, p.Q);         break;
            case FilterType::LowPass:   d.c0 = matched::lowpass      (p.freq, fs, p.Q);         break;
            case FilterType::BandPass:  d.c0 = matched::bandpass     (p.freq, fs, p.Q);         break;
            case FilterType::Notch:     d.c0 = matched::notch        (p.freq, fs, p.Q);         break;
            case FilterType::AllPass:   d.c0 = matched::allpass      (p.freq, fs, p.Q);         break;
            case FilterType::Tilt:      break;   // handled above (two-stage shelves)
        }
    }
    return d;
}

inline std::complex<double> evalCoeffs (const BiquadCoeffs& c, double w) noexcept
{
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
}

// Race-free GUI response at digital w — computed purely from a caller-owned BandParams snapshot.
inline std::complex<double> bandResponse (const BandParams& p, double fs, double w) noexcept
{
    if (! p.on) return { 1.0, 0.0 };
    const BandDesign d = designBand (p, fs);
    std::complex<double> h = evalCoeffs (d.c0, w);
    if (d.twoStage) h *= evalCoeffs (d.c1, w);
    return h;
}

//==============================================================================
// teq::EqBand — one EQ band. Owns its parameter smoothers, picks the right design, and
// processes a block in place (mono or stereo). Two engines under one band:
//   * static treatment band  -> matched biquad(s) (Nyquist-accurate; 24 dB/oct = 2 sections)
//   * swept / search band    -> zero-delay SVF    (clean under a fast fc sweep)
// Smoothers advance in closed form per block, so coefficients are recomputed once per block
// while freq/Q/gain still move in real time — no zipper, no per-sample biquad redesign.
// `response()` always reports the matched (display) curve so the GUI stays honest near Nyquist.
class EqBand
{
public:
    static constexpr int kMaxChannels = Svf::kMaxChannels;

    void prepare (double sampleRate, int numChannels, double smoothMs = 30.0) noexcept
    {
        fs = sampleRate;
        ch = numChannels < 1 ? 1 : (numChannels > kMaxChannels ? kMaxChannels : numChannels);
        freqS.prepare (fs, smoothMs);
        qS.prepare    (fs, smoothMs);
        gainS.prepare (fs, smoothMs);
        freqS.snap (p.freq); qS.snap (p.Q); gainS.snap (p.gainDb);
        svf.prepare (fs, ch);
        updateCoeffs();
        recomputePending = false;
        initialized = false;
        wasActive = false;
        reset();
    }

    void reset() noexcept
    {
        for (int c = 0; c < kMaxChannels; ++c) { bq0[c].reset(); bq1[c].reset(); }
        svf.reset();
    }

    // Call from the SAME thread as processBlock() (typically the audio thread, where the host
    // adapter reads its atomic parameters), or synchronise externally — the engine takes no
    // internal lock. The FIRST call SNAPS (a freshly loaded plugin starts at its settings, no
    // ramp); later calls smooth freq/Q/gain. Identical params are a no-op (recompute stays skipped
    // even if called every block). type/slope/swept/on apply on the next block.
    void setParams (const BandParams& npIn) noexcept
    {
        auto finiteOr = [] (double x, double fb) noexcept { return std::isfinite (x) ? x : fb; };
        BandParams np = npIn;
        np.freq   = std::clamp (finiteOr (np.freq, 1000.0), 10.0, 0.49 * fs);
        np.Q      = std::clamp (finiteOr (np.Q, 1.0), 0.05, 40.0);
        np.gainDb = std::clamp (finiteOr (np.gainDb, 0.0), -30.0, 30.0);

        if (! initialized)
        {
            p = np;
            freqS.snap (p.freq); qS.snap (p.Q); gainS.snap (p.gainDb);
            initialized = true;
            recomputePending = true;
        }
        else if (! (np == p))
        {
            p = np;
            freqS.setTarget (p.freq); qS.setTarget (p.Q); gainS.setTarget (p.gainDb);
            recomputePending = true;
        }
    }

    const BandParams& params() const noexcept { return p; }

    // Audio thread. In-place. RT-safe (no alloc / lock / IO).
    void processBlock (float* const* channels, int numChannels, int numSamples) noexcept
    {
        if (! p.on || numSamples <= 0)
        {
            if (! p.on && wasActive) { reset(); wasActive = false; }   // clear the tail so re-enabling doesn't pop
            return;
        }
        wasActive = true;

        freqS.advance (numSamples);
        qS.advance    (numSamples);
        gainS.advance (numSamples);

        // Recompute only when something actually moves — a static, settled band skips the
        // per-block tan/exp/sin/sqrt entirely.
        const bool moving = ! (freqS.settled() && qS.settled() && gainS.settled());
        if (recomputePending || moving) { updateCoeffs(); recomputePending = moving; }

        const int nc = numChannels < ch ? numChannels : ch;

        if (p.swept)
            for (int c = 0; c < nc; ++c)
            {
                float* d = channels[c];
                for (int n = 0; n < numSamples; ++n) d[n] = svf.processSample (c, d[n]);
            }
        else if (twoStage)
            for (int c = 0; c < nc; ++c)
            {
                float* d = channels[c];
                for (int n = 0; n < numSamples; ++n) d[n] = bq1[c].processSample (bq0[c].processSample (d[n]));
            }
        else
            for (int c = 0; c < nc; ++c)
            {
                float* d = channels[c];
                for (int n = 0; n < numSamples; ++n) d[n] = bq0[c].processSample (d[n]);
            }

        flushState();   // per-block denormal guard
    }

    // Complex frequency response at digital w (rad/sample) from the band's current smoothed state.
    // For a race-free GUI curve prefer the free bandResponse() with your own BandParams snapshot.
    std::complex<double> response (double w) const noexcept
    {
        if (! p.on) return { 1.0, 0.0 };
        std::complex<double> h = evalCoeffs (coeffs0, w);
        if (twoStage) h *= evalCoeffs (coeffs1, w);
        return h;
    }

private:
    void updateCoeffs() noexcept
    {
        BandParams sp = p;
        sp.freq = freqS.value(); sp.Q = qS.value(); sp.gainDb = gainS.value();

        const BandDesign d = designBand (sp, fs);

        // A topology switch (swept<->static, or 24<->12 dB/oct) changes WHICH filter runs — clear
        // all state so a re-activated bq1/svf never resumes a stale tail (a click). Rare event;
        // costs nothing on steady state. Matters for TabbyEQ: search->treat toggles `swept`.
        if (d.twoStage != twoStage || p.swept != lastSwept) reset();

        twoStage  = d.twoStage;
        lastSwept = p.swept;
        coeffs0   = d.c0;
        coeffs1   = d.c1;

        for (int c = 0; c < ch; ++c) { bq0[c].setCoeffs (coeffs0); if (twoStage) bq1[c].setCoeffs (coeffs1); }
        if (p.swept) svf.setParams (p.type, sp.freq, sp.Q, sp.gainDb);   // swept = single SVF stage (12 dB/oct)
    }

    void flushState() noexcept
    {
        if (p.swept) svf.flushDenormals();
        else for (int c = 0; c < ch; ++c) { bq0[c].flushDenormals(); if (twoStage) bq1[c].flushDenormals(); }
    }

    BandParams p;
    double fs = 44100.0;
    int    ch = 2;
    bool   twoStage = false;
    bool   recomputePending = true;
    bool   initialized = false;
    bool   wasActive = false;
    bool   lastSwept = false;

    Smoother freqS, qS, gainS;
    Biquad   bq0[kMaxChannels], bq1[kMaxChannels];
    Svf      svf;
    BiquadCoeffs coeffs0, coeffs1;
};

} // namespace teq
