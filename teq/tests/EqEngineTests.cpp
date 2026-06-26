// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

// Verifies the runtime engine by actually pushing audio through it: measured steady-state gain
// of a sine must agree with the analytic magnitudeDb() readout, smoothing converges, the swept
// SVF band matches the matched band, a 24 dB/oct HP rolls off ~24 dB/oct, and process() is clean
// (silence stays silent, no NaN). JUCE-free.

#include "TestUtil.h"

#include <teq/EqBand.h>
#include <teq/EqEngine.h>
#include <teq/Smoother.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace teq;

namespace
{
    // Steady-state gain (dB) of a mono in-place processing callback at frequency f.
    template <class Apply>
    double sineGainDb (Apply&& apply, double f, double fs, int warm = 600)
    {
        const int block = 256;                 // warm=600 -> ~3.2 s >> 30 ms smoothing + filter ring
        const double amp = 0.25, dp = 2.0 * kPi * f / fs;
        std::vector<float> buf ((size_t) block);
        double phase = 0.0;
        auto fill = [&] { for (int n = 0; n < block; ++n) { buf[(size_t) n] = (float) (amp * std::sin (phase));
                                                            phase += dp; if (phase > 2.0 * kPi) phase -= 2.0 * kPi; } };

        for (int b = 0; b < warm; ++b) { fill(); float* p = buf.data(); apply (&p, 1, block); }

        fill();
        std::vector<float> in (buf.begin(), buf.end());
        { float* p = buf.data(); apply (&p, 1, block); }

        double ir = 0.0, orr = 0.0;
        for (int n = 0; n < block; ++n)
        {
            ir  += (double) in[(size_t) n]  * in[(size_t) n];
            orr += (double) buf[(size_t) n] * buf[(size_t) n];
        }
        return 20.0 * std::log10 (std::sqrt (orr / std::max (1e-30, ir)));
    }

    bool anyNaN (const float* d, int n) { for (int i = 0; i < n; ++i) if (! std::isfinite (d[i])) return true; return false; }
}

void runEqEngineTests()
{
    using namespace teqtest;
    const double fs = 48000.0;

    group ("Smoother: converges to target, snap settles");
    {
        Smoother s; s.prepare (fs, 10.0); s.snap (0.0); s.setTarget (1.0);
        s.advance (48000);
        expectNear (s.value(), 1.0, 1e-3, "advance(1s) converges");
        s.snap (5.0);
        expectTrue (s.settled(), "snap settles");
    }

    group ("EqBand bell: measured gain == response() at centre, ~unity far off");
    {
        EqBand band; band.prepare (fs, 1);
        BandParams p; p.on = true; p.type = FilterType::Bell; p.freq = 1000.0; p.Q = 2.0; p.gainDb = 6.0;
        band.setParams (p);
        const double meas = sineGainDb ([&] (float* const* ch, int nc, int n) { band.processBlock (ch, nc, n); }, 1000.0, fs);
        const double resp = 20.0 * std::log10 (std::abs (band.response (2.0 * kPi * 1000.0 / fs)));
        std::printf ("      bell @1k: measured=%.3f dB  response=%.3f dB\n", meas, resp);
        expectNear (meas, 6.0, 0.4,  "measured +6 dB");
        expectNear (resp, 6.0, 0.05, "response +6 dB");
        expectNear (meas, resp, 0.4, "measured ~ response");

        EqBand far; far.prepare (fs, 1);
        BandParams q; q.on = true; q.type = FilterType::Bell; q.freq = 1000.0; q.Q = 4.0; q.gainDb = 9.0;
        far.setParams (q);
        const double low = sineGainDb ([&] (float* const* ch, int nc, int n) { far.processBlock (ch, nc, n); }, 60.0, fs);
        expectNear (low, 0.0, 0.3, "unity an octave+ away");
    }

    group ("EqBand swept (SVF) bell tracks the same +6 dB");
    {
        EqBand band; band.prepare (fs, 1);
        BandParams p; p.on = true; p.type = FilterType::Bell; p.freq = 2000.0; p.Q = 3.0; p.gainDb = 6.0; p.swept = true;
        band.setParams (p);
        const double meas = sineGainDb ([&] (float* const* ch, int nc, int n) { band.processBlock (ch, nc, n); }, 2000.0, fs);
        std::printf ("      swept bell @2k: measured=%.3f dB\n", meas);
        expectNear (meas, 6.0, 0.5, "SVF bell +6 dB");
    }

    group ("EqBand HP 24 dB/oct rolls off ~24 dB/octave in the stopband");
    {
        EqBand band; band.prepare (fs, 1);
        BandParams p; p.on = true; p.type = FilterType::HighPass; p.freq = 1000.0; p.Q = 0.707; p.slope = 24;
        band.setParams (p);
        auto ap = [&] (float* const* ch, int nc, int n) { band.processBlock (ch, nc, n); };
        const double g125 = sineGainDb (ap, 125.0, fs);
        const double g250 = sineGainDb (ap, 250.0, fs);
        std::printf ("      HP24: g(125)=%.2f  g(250)=%.2f  slope=%.1f dB/oct\n", g125, g250, g250 - g125);
        expectTrue ((g250 - g125) > 20.0 && (g250 - g125) < 28.0, "~24 dB/oct");
    }

    group ("EqEngine: two bells cascade, measured == magnitudeDb");
    {
        EqEngine eng; eng.prepare (fs, 512, 1);
        BandParams a; a.on = true; a.type = FilterType::Bell; a.freq = 300.0;  a.Q = 1.0; a.gainDb =  4.0;
        BandParams b; b.on = true; b.type = FilterType::Bell; b.freq = 3000.0; b.Q = 1.0; b.gainDb = -3.0;
        eng.setBand (0, a); eng.setBand (1, b);
        const double m = sineGainDb ([&] (float* const* ch, int nc, int n) { eng.process (ch, nc, n); }, 300.0, fs);
        const double r = eng.magnitudeDb (300.0);
        std::printf ("      @300: measured=%.3f dB  magnitudeDb=%.3f dB\n", m, r);
        expectNear (m, r, 0.4, "measured ~ magnitudeDb");
        expectNear (r, 4.0, 0.6, "~+4 dB at band-0 centre");
    }

    group ("swept SVF BandPass has unity gain at centre");
    {
        EqBand band; band.prepare (fs, 1);
        BandParams p; p.on = true; p.type = FilterType::BandPass; p.freq = 1000.0; p.Q = 4.0; p.swept = true;
        band.setParams (p);
        const double meas = sineGainDb ([&] (float* const* ch, int nc, int n) { band.processBlock (ch, nc, n); }, 1000.0, fs);
        std::printf ("      swept BP @1k Q=4: measured=%.3f dB\n", meas);
        expectNear (meas, 0.0, 0.5, "unity (0 dB) at centre");
    }

    group ("swept SVF high shelf is Butterworth (plateau == gain, Q-independent)");
    {
        auto plateau = [&] (double Q)
        {
            EqBand band; band.prepare (fs, 1);
            BandParams p; p.on = true; p.type = FilterType::HighShelf; p.freq = 3000.0; p.Q = Q; p.gainDb = 12.0; p.swept = true;
            band.setParams (p);
            return sineGainDb ([&] (float* const* ch, int nc, int n) { band.processBlock (ch, nc, n); }, 14000.0, fs);
        };
        const double g1 = plateau (1.0), g4 = plateau (4.0);
        std::printf ("      swept HS plateau @14k: Q=1 -> %.3f dB, Q=4 -> %.3f dB\n", g1, g4);
        expectNear (g1, 12.0, 0.6, "plateau ~ +12 dB");
        expectNear (g1, g4,   0.2, "Q ignored for shelves");
    }

    group ("parameter safety: bad Q / huge gain / NaN / Inf do not produce NaN");
    {
        const double inf = std::numeric_limits<double>::infinity();
        EqEngine eng; eng.prepare (fs, 256, 1);
        BandParams p; p.on = true; p.type = FilterType::Bell;
        p.freq = std::nan (""); p.Q = 0.0; p.gainDb = inf;       // NaN freq, Q=0, +Inf gain
        eng.setBand (0, p);
        const int n = 256;
        std::vector<float> buf ((size_t) n);
        for (int i = 0; i < n; ++i) buf[(size_t) i] = 0.2f * (float) std::sin (2.0 * kPi * 1000.0 * i / fs);
        float* ch[1] = { buf.data() };
        for (int b = 0; b < 20; ++b) eng.process (ch, 1, n);
        expectTrue (! anyNaN (buf.data(), n),                                       "sanitised params -> finite output");
        expectTrue (std::isfinite (eng.magnitudeDb (1000.0)),                       "magnitudeDb finite");
        expectTrue (std::isfinite (EqEngine::magnitudeDbFor (&p, 1, 1000.0, fs)),   "magnitudeDbFor finite on bad params");
    }

    group ("denormal flush: tail decays to exact zero after silence");
    {
        EqBand band; band.prepare (fs, 1);
        BandParams p; p.on = true; p.type = FilterType::Bell; p.freq = 1000.0; p.Q = 2.0; p.gainDb = 6.0;
        band.setParams (p);
        const int n = 64;
        std::vector<float> buf ((size_t) n, 0.0f);
        buf[0] = 1.0f;                                           // impulse, then silence
        float* ch[1] = { buf.data() };
        band.processBlock (ch, 1, n);
        for (int b = 0; b < 200; ++b) { for (int i = 0; i < n; ++i) buf[(size_t) i] = 0.0f; band.processBlock (ch, 1, n); }
        double tail = 0.0; for (int i = 0; i < n; ++i) tail += std::fabs (buf[(size_t) i]);
        expectTrue (tail == 0.0, "tail flushed to exact zero (no denormal residue)");
    }

    group ("first setParams snaps (no ramp from defaults on load)");
    {
        EqBand band; band.prepare (fs, 1);
        BandParams p; p.on = true; p.type = FilterType::Bell; p.freq = 1000.0; p.Q = 2.0; p.gainDb = 6.0;
        band.setParams (p);
        // measured after only 2 blocks (< 30 ms smoothing): snapped -> already +6 dB, not mid-ramp.
        const double meas = sineGainDb ([&] (float* const* ch, int nc, int n) { band.processBlock (ch, nc, n); }, 1000.0, fs, 2);
        std::printf ("      gain after 2 blocks: %.3f dB\n", meas);
        expectNear (meas, 6.0, 0.5, "target reached immediately (no ramp from defaults)");
    }

    group ("stateless magnitudeDbFor matches the live magnitudeDb");
    {
        EqEngine eng; eng.prepare (fs, 512, 1);
        BandParams arr[EqEngine::kMaxBands] {};
        arr[0].on = true; arr[0].type = FilterType::Bell;      arr[0].freq = 300.0;  arr[0].Q = 1.0; arr[0].gainDb =  4.0;
        arr[1].on = true; arr[1].type = FilterType::HighShelf; arr[1].freq = 8000.0;                 arr[1].gainDb = -5.0;
        eng.setBand (0, arr[0]); eng.setBand (1, arr[1]);
        const int n = 64; std::vector<float> z ((size_t) n, 0.0f); float* ch[1] = { z.data() };
        for (int b = 0; b < 4; ++b) eng.process (ch, 1, n);     // let the live coeffs update
        for (double f : { 100.0, 300.0, 1000.0, 8000.0, 15000.0 })
        {
            const double live      = eng.magnitudeDb (f);
            const double stateless = EqEngine::magnitudeDbFor (arr, EqEngine::kMaxBands, f, fs);
            expectNear (stateless, live, 1e-6, "stateless == live @" + std::to_string ((int) f));
        }
    }

    group ("swept HP slope=24 is single-stage: audio slope == response slope (~12 dB/oct)");
    {
        EqBand band; band.prepare (fs, 1);
        BandParams p; p.on = true; p.type = FilterType::HighPass; p.freq = 1000.0; p.Q = 0.707; p.slope = 24; p.swept = true;
        band.setParams (p);
        auto ap = [&] (float* const* ch, int nc, int n) { band.processBlock (ch, nc, n); };
        const double a125 = sineGainDb (ap, 125.0, fs), a250 = sineGainDb (ap, 250.0, fs);
        const double audioSlope = a250 - a125;
        const double r125 = 20.0 * std::log10 (std::abs (band.response (2.0 * kPi * 125.0 / fs)));
        const double r250 = 20.0 * std::log10 (std::abs (band.response (2.0 * kPi * 250.0 / fs)));
        const double respSlope = r250 - r125;
        std::printf ("      swept HP24: audioSlope=%.1f  responseSlope=%.1f dB/oct\n", audioSlope, respSlope);
        expectTrue (audioSlope > 9.0 && audioSlope < 15.0, "audio ~12 dB/oct (single SVF, not 24)");
        expectNear (respSlope, audioSlope, 2.5,            "response agrees with audio (no 24-vs-12 mismatch)");
    }

    group ("turning a band off resets state (no stale tail on re-enable)");
    {
        EqBand band; band.prepare (fs, 1);
        BandParams on; on.on = true; on.type = FilterType::Bell; on.freq = 1000.0; on.Q = 2.0; on.gainDb = 6.0;
        band.setParams (on);
        const int n = 64; std::vector<float> buf ((size_t) n, 0.0f); buf[0] = 1.0f; float* ch[1] = { buf.data() };
        band.processBlock (ch, 1, n);                              // build state from an impulse
        BandParams off = on; off.on = false; band.setParams (off);
        for (int i = 0; i < n; ++i) buf[(size_t) i] = 0.0f;
        band.processBlock (ch, 1, n);                              // off block -> resets state
        band.setParams (on);                                      // re-enable
        for (int i = 0; i < n; ++i) buf[(size_t) i] = 0.0f;
        band.processBlock (ch, 1, n);                              // silent in -> must be silent out
        double tail = 0.0; for (int i = 0; i < n; ++i) tail += std::fabs (buf[(size_t) i]);
        expectTrue (tail == 0.0, "re-enabled band starts from a clean state");
    }

    group ("topology toggle (swept<->static, 24<->12) clears stale state");
    {
        const int n = 64; std::vector<float> buf ((size_t) n, 0.0f); float* ch[1] = { buf.data() };

        EqBand band; band.prepare (fs, 1);
        BandParams sw; sw.on = true; sw.type = FilterType::Bell; sw.freq = 1000.0; sw.Q = 4.0; sw.gainDb = 6.0; sw.swept = true;
        band.setParams (sw);
        buf[0] = 1.0f; band.processBlock (ch, 1, n);                                          // SVF builds state
        BandParams st = sw; st.swept = false; band.setParams (st);
        for (int i = 0; i < n; ++i) buf[(size_t) i] = 0.0f; band.processBlock (ch, 1, n);     // -> static (resets)
        band.setParams (sw);                                                                 // -> swept again
        for (int i = 0; i < n; ++i) buf[(size_t) i] = 0.0f; band.processBlock (ch, 1, n);
        double t1 = 0.0; for (int i = 0; i < n; ++i) t1 += std::fabs (buf[(size_t) i]);
        expectTrue (t1 == 0.0, "no stale SVF tail after swept<->static");

        EqBand hp; hp.prepare (fs, 1);
        BandParams p24; p24.on = true; p24.type = FilterType::HighPass; p24.freq = 1000.0; p24.Q = 0.707; p24.slope = 24;
        hp.setParams (p24);
        for (int i = 0; i < n; ++i) buf[(size_t) i] = 0.0f; buf[0] = 1.0f; hp.processBlock (ch, 1, n);   // bq0+bq1 state
        BandParams p12 = p24; p12.slope = 12; hp.setParams (p12);
        for (int i = 0; i < n; ++i) buf[(size_t) i] = 0.0f; hp.processBlock (ch, 1, n);                  // -> single (resets)
        hp.setParams (p24);                                                                             // -> 24 again
        for (int i = 0; i < n; ++i) buf[(size_t) i] = 0.0f; hp.processBlock (ch, 1, n);
        double t2 = 0.0; for (int i = 0; i < n; ++i) t2 += std::fabs (buf[(size_t) i]);
        expectTrue (t2 == 0.0, "no stale bq1 tail after 24<->12 dB/oct");
    }

    group ("EqEngine: silence stays silent, no NaN (stereo)");
    {
        EqEngine eng; eng.prepare (fs, 256, 2);
        BandParams p; p.on = true; p.type = FilterType::HighShelf; p.freq = 8000.0; p.gainDb = 12.0;
        eng.setBand (0, p);
        const int n = 256;
        std::vector<float> L ((size_t) n, 0.0f), R ((size_t) n, 0.0f);
        float* ch[2] = { L.data(), R.data() };
        for (int b = 0; b < 10; ++b) eng.process (ch, 2, n);
        double energy = 0.0;
        for (int i = 0; i < n; ++i) energy += std::fabs (L[(size_t) i]) + std::fabs (R[(size_t) i]);
        expectTrue (energy == 0.0, "silence stays silent");
        expectTrue (! anyNaN (L.data(), n) && ! anyNaN (R.data(), n), "no NaN");
    }
}
