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
struct BandDesign
{
    static constexpr int kMaxSections = 8;   // up to 96 dB/oct (16-pole HP/LP = 8 biquads)
    BiquadCoeffs sec[kMaxSections];
    int n = 1;
};

inline BandDesign designBand (const BandParams& in, double fs) noexcept
{
    auto finiteOr = [] (double x, double fb) noexcept { return std::isfinite (x) ? x : fb; };
    BandParams p = in;
    p.freq   = std::clamp (finiteOr (p.freq, 1000.0), 10.0, 0.49 * fs);
    p.Q      = std::clamp (finiteOr (p.Q, 1.0), 0.05, 40.0);
    p.gainDb = std::clamp (finiteOr (p.gainDb, 0.0), -30.0, 30.0);

    BandDesign d;
    const bool isCut = (p.type == FilterType::HighPass || p.type == FilterType::LowPass);

    if (isCut && ! p.swept)
    {
        // Butterworth cascade: order = slope/6 poles {1,2,4,6,8,12,16}; sections = order/2 (+ a
        // first-order section if the order is odd, i.e. 6 dB/oct). Per-section Qs are the standard
        // Butterworth pole Qs, each realised by a Nyquist-matched biquad.
        const int  order = std::clamp (p.slope / 6, 1, 16);
        const bool isHP  = (p.type == FilterType::HighPass);
        int idx = 0;
        if (order % 2 == 1)
            d.sec[idx++] = isHP ? matched::highpass1 (p.freq, fs) : matched::lowpass1 (p.freq, fs);
        const int nSec = order / 2;
        for (int k = 1; k <= nSec && idx < BandDesign::kMaxSections; ++k)
        {
            const double Qk = 1.0 / (2.0 * std::cos ((2.0 * k - 1.0) * kPi / (2.0 * order)));
            d.sec[idx++] = isHP ? matched::highpass (p.freq, fs, Qk) : matched::lowpass (p.freq, fs, Qk);
        }
        d.n = idx;
    }
    else if (p.type == FilterType::Tilt)
    {
        d.sec[0] = matched::lowShelfDb  (p.freq, fs, -p.gainDb);   // lows down
        d.sec[1] = matched::highShelfDb (p.freq, fs,  p.gainDb);   // highs up -> spectral tilt about f0
        d.n = 2;
    }
    else
    {
        switch (p.type)
        {
            case FilterType::Bell:      d.sec[0] = matched::peakingDb  (p.freq, fs, p.Q, p.gainDb); break;
            case FilterType::LowShelf:  d.sec[0] = matched::lowShelfDb  (p.freq, fs, p.gainDb);     break;
            case FilterType::HighShelf: d.sec[0] = matched::highShelfDb (p.freq, fs, p.gainDb);     break;
            case FilterType::HighPass:  d.sec[0] = matched::highpass     (p.freq, fs, p.Q);         break;  // swept fallback (single)
            case FilterType::LowPass:   d.sec[0] = matched::lowpass      (p.freq, fs, p.Q);         break;  // swept fallback (single)
            case FilterType::BandPass:  d.sec[0] = matched::bandpass     (p.freq, fs, p.Q);         break;
            case FilterType::Notch:     d.sec[0] = matched::notch        (p.freq, fs, p.Q);         break;
            case FilterType::AllPass:   d.sec[0] = matched::allpass      (p.freq, fs, p.Q);         break;
            case FilterType::Tilt:      break;   // handled above (two shelves)
        }
        d.n = 1;
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
    std::complex<double> h { 1.0, 0.0 };
    for (int s = 0; s < d.n; ++s) h *= evalCoeffs (d.sec[s], w);
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
    static constexpr int kMaxSections = BandDesign::kMaxSections;

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
        for (int c = 0; c < kMaxChannels; ++c)
            for (int s = 0; s < kMaxSections; ++s) bq[s][c].reset();
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
        else
            for (int c = 0; c < nc; ++c)
            {
                float* d = channels[c];
                for (int n = 0; n < numSamples; ++n)
                {
                    float x = d[n];
                    for (int s = 0; s < designN; ++s) x = bq[s][c].processSample (x);
                    d[n] = x;
                }
            }

        flushState();   // per-block denormal guard
    }

    // Complex frequency response at digital w (rad/sample) from the band's current smoothed state.
    // For a race-free GUI curve prefer the free bandResponse() with your own BandParams snapshot.
    std::complex<double> response (double w) const noexcept
    {
        if (! p.on) return { 1.0, 0.0 };
        std::complex<double> h { 1.0, 0.0 };
        for (int s = 0; s < designN; ++s) h *= evalCoeffs (coeffs[s], w);
        return h;
    }

private:
    void updateCoeffs() noexcept
    {
        BandParams sp = p;
        sp.freq = freqS.value(); sp.Q = qS.value(); sp.gainDb = gainS.value();

        const BandDesign d = designBand (sp, fs);

        // A topology switch (section count changed, or swept<->static) changes WHICH filters run —
        // clear all state so a re-activated section never resumes a stale tail (a click). Rare event;
        // costs nothing on steady state. Matters for TabbyEQ: search->treat toggles `swept`.
        if (d.n != designN || p.swept != lastSwept) reset();

        designN   = d.n;
        lastSwept = p.swept;
        for (int s = 0; s < d.n; ++s) coeffs[s] = d.sec[s];

        for (int c = 0; c < ch; ++c)
            for (int s = 0; s < d.n; ++s) bq[s][c].setCoeffs (coeffs[s]);
        if (p.swept) svf.setParams (p.type, sp.freq, sp.Q, sp.gainDb);   // swept = single SVF stage (12 dB/oct)
    }

    void flushState() noexcept
    {
        if (p.swept) svf.flushDenormals();
        else for (int c = 0; c < ch; ++c)
            for (int s = 0; s < designN; ++s) bq[s][c].flushDenormals();
    }

    BandParams p;
    double fs = 44100.0;
    int    ch = 2;
    int    designN = 1;
    bool   recomputePending = true;
    bool   initialized = false;
    bool   wasActive = false;
    bool   lastSwept = false;

    Smoother freqS, qS, gainS;
    Biquad   bq[kMaxSections][kMaxChannels];
    Svf      svf;
    BiquadCoeffs coeffs[kMaxSections];
};

} // namespace teq
