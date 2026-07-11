// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.
//
// ADVERSARIAL theory suite for the eqview headers (PlotMap + SpectrumPane) — the crew's attempt to
// REFUTE the code (house policy: tests come from THEORY, never from the implementation). Every
// expectation below is derived from first principles — the DFT of the SYMMETRIC Hann window via
// the Dirichlet kernel, hand-computed affine/log-axis constants, and the documented smoothing/fade
// recurrences evaluated in double — and only then compared against the pipeline's output through
// its public API. A failure here means the CODE deviates from theory (or a derivation is wrong —
// both get investigated, neither gets ignored). Case authors: codex / deepseek / opus as marked.

#include "eqview/SpectrumPane.h"

#include <cmath>
#include <complex>
#include <cstdio>

static int failures = 0;

static void check (bool ok, const char* what)
{
    if (! ok) { std::printf ("FAIL: %s\n", what); ++failures; }
    else        std::printf ("ok:   %s\n", what);
}

static bool nearEq (double a, double b, double tol) { return std::abs (a - b) <= tol; }

//==============================================================================
// THEORY: DFT of the symmetric Hann window, from first principles.
//   D(r)  = sum_{n=0}^{N-1} e^{-j2πrn/N} = e^{-jπr(N-1)/N} · sin(πr)/sin(πr/N)   (Dirichlet)
//   w[n]  = 0.5 - 0.25·e^{+j2πn/(N-1)} - 0.25·e^{-j2πn/(N-1)}
//   W(r)  = 0.5·D(r) - 0.25·D(r-h) - 0.25·D(r+h),  h = N/(N-1)   (NOT the periodic Hann's h=1!)
//   sine A·sin(2πmn/N):  X[k] = A·(W(k-m) - W(k+m)) / (2j)
// The pipeline reads g = |X[k]| / (N/4) and dB = max(-120, 20·log10 g).
namespace theory
{
    constexpr int    N  = eqview::SpectrumPane::fftSize;
    constexpr double pi = 3.14159265358979323846;

    static std::complex<double> D (double r)
    {
        // r == 0 (mod N): the sum degenerates to N (all terms 1 at r=0).
        const double rm = std::remainder (r, (double) N);
        if (std::abs (rm) < 1e-12) return { (double) N, 0.0 };
        const double mag = std::sin (pi * r) / std::sin (pi * r / (double) N);   // may be NEGATIVE —
        const double ph  = -pi * r * (double) (N - 1) / (double) N;               // std::polar would NaN
        return { mag * std::cos (ph), mag * std::sin (ph) };                      // on libc++, so build it by hand
    }

    static std::complex<double> W (double r)
    {
        const double h = (double) N / (double) (N - 1);
        return 0.5 * D (r) - 0.25 * D (r - h) - 0.25 * D (r + h);
    }

    // |X[k]| for a unit sine at (possibly fractional) bin m
    static double sineBinMag (double m, int k)
    {
        const std::complex<double> X = (W ((double) k - m) - W ((double) k + m)) / std::complex<double> (0.0, 2.0);
        return std::abs (X);
    }

    static double binDb (double mag) { const double g = mag / ((double) N * 0.25);
                                       return g > 0.0 ? std::max (-120.0, 20.0 * std::log10 (g)) : -120.0; }

    static double smooth1 (double target) { return -120.0 + 0.25 * (target + 120.0); }   // one tick from the floor
}

//==============================================================================
// Probe helpers — read a BIN's fill/peak dB through the public column API: a map whose log-centre
// sits exactly on the bin's frequency (x=width/2 -> f = sqrt(fmin·fmax) = fk) with a span narrow
// enough that the centre column runs the interpolation branch with t≈0. Scale: 1 dB/px.
struct BinProbe { double fill = 0.0, peak = 0.0; };

static BinProbe probeBin (const eqview::SpectrumPane& p, double fs, double fHz)
{
    eqview::PlotMap pm;
    pm.width = 900.0f; pm.height = 160.0f; pm.plotBottom = 160.0f;
    pm.freqMin = fHz * 0.9; pm.freqMax = fHz / 0.9;
    pm.specTop = 20.0; pm.specBottom = -140.0;                       // 160 dB / 160 px = 1 dB/px
    BinProbe out; double bestDx = 1.0e9; const double xT = 450.0;
    p.buildColumns (pm, fs, 0.0, 1000.0, [&] (int, float x, float yF, float yP)
    {
        const double dx = std::abs ((double) x - xT);
        if (dx < bestDx) { bestDx = dx; out.fill = 20.0 - (double) yF; out.peak = 20.0 - (double) yP; }
    });
    return out;
}

static void feedSine (eqview::SpectrumPane& p, double binM, int ticks, double amp = 1.0)
{
    for (int t = 0; t < ticks; ++t)
    {
        float* in = p.frameInput();
        for (int i = 0; i < theory::N; ++i)
            in[i] = (float) (amp * std::sin (2.0 * theory::pi * binM * (double) i / (double) theory::N));
        p.ingest();
    }
}

int main()
{
    using Pane = eqview::SpectrumPane;
    const double fs = 48000.0;

    //==========================================================================================
    // [codex #1] PlotMap log/affine algebra on awkward geometry — all constants hand-derived.
    {
        eqview::PlotMap m;
        m.width = 1234.5f; m.height = 333.25f; m.plotBottom = 317.75f;
        m.freqMin = 17.5; m.freqMax = 30100.0; m.dbRange = 18.0; m.specTop = 12.0; m.specBottom = -108.0;
        check (nearEq (m.xToFreq (-10.0f), 17.5, 2e-6 * 17.5),        "codex1: xToFreq clamps low");
        check (nearEq (m.xToFreq (123.45f), 36.8630184293, 2e-6 * 36.863), "codex1: xToFreq at t=0.1 (hand: 17.5*1720^0.1)");
        check (nearEq (m.xToFreq (2000.0f), 30100.0, 2e-6 * 30100.0), "codex1: xToFreq clamps high");
        check (nearEq (m.freqToX (1.0), 0.0, 1e-3),                   "codex1: freqToX clamps low to 0");
        check (nearEq (m.freqToX (31.25), 96.0776224848, 1.5e-3),     "codex1: freqToX(31.25) (hand log ratio)");
        check (nearEq (m.freqToX (99999.0), 1234.5, 1e-3),            "codex1: freqToX clamps high to width");
        check (nearEq (m.dbToY (36.0), -158.875, 1e-3),               "codex1: dbToY overshoots ABOVE the top (unclamped)");
        check (nearEq (m.dbToY (18.0), 0.0, 1e-3),                    "codex1: dbToY(+range) == 0");
        check (nearEq (m.dbToY (0.0), 158.875, 1e-3),                 "codex1: dbToY(0) == plotBottom/2");
        check (nearEq (m.dbToY (-54.0), 635.5, 1e-3),                 "codex1: dbToY overshoots BELOW (2*plotBottom)");
        check (nearEq (m.specDbToY (100.0), 0.0, 1e-3),               "codex1: spec clamps above top");
        check (nearEq (m.specDbToY (-48.0), 166.625, 1e-3),           "codex1: spec mid (hand: t=0.5)");
        check (nearEq (m.specDbToY (-200.0), 333.25, 1e-3),           "codex1: spec clamps below bottom");
    }

    //==========================================================================================
    // [codex #2] SYMMETRIC-Hann exact-bin leakage signature. The symmetric window (h=N/(N-1))
    // leaks where the DFT-periodic one would be silent: bins ±2 sit ~11 dB above the periodic
    // prediction. One ingest from the floor; expectations from the W(r) kernel + one smooth tick.
    {
        Pane p; feedSine (p, 100.0, 1);
        for (int k = 98; k <= 102; ++k)
        {
            const double expect = theory::smooth1 (theory::binDb (theory::sineBinMag (100.0, k)));
            const double got    = probeBin (p, fs, (double) k * fs / theory::N).fill;
            char label[96]; std::snprintf (label, sizeof (label), "codex2: symmetric-Hann signature at bin %d", k);
            check (nearEq (got, expect, 0.08), label);
        }
    }

    //==========================================================================================
    // [codex #3] Half-bin sine — worst-case scalloping. Both straddling bins must read the
    // analytic ~-1.43 dB scallop, and must agree with each other.
    {
        Pane p; feedSine (p, 100.5, 1);
        const double e100 = theory::smooth1 (theory::binDb (theory::sineBinMag (100.5, 100)));
        const double e101 = theory::smooth1 (theory::binDb (theory::sineBinMag (100.5, 101)));
        const double g100 = probeBin (p, fs, 100.0 * fs / theory::N).fill;
        const double g101 = probeBin (p, fs, 101.0 * fs / theory::N).fill;
        check (nearEq (g100, e100, 0.05), "codex3: half-bin scalloping, lower bin");
        check (nearEq (g101, e101, 0.05), "codex3: half-bin scalloping, upper bin");
        check (nearEq (g100, g101, 0.02), "codex3: half-bin symmetry between straddling bins");
    }

    //==========================================================================================
    // [codex #4] Two-tone resolution — DFT linearity: X[k] is the COMPLEX sum of both tones'
    // kernels; the 4-bin-spaced valley must match theory, not a magnitude-sum shortcut.
    {
        Pane p;
        {
            float* in = p.frameInput();
            for (int i = 0; i < theory::N; ++i)
                in[i] = (float) (std::sin (2.0 * theory::pi * 100.0 * i / theory::N)
                               + std::sin (2.0 * theory::pi * 104.0 * i / theory::N));
            p.ingest();
        }
        auto twoToneDb = [] (int k)
        {
            const std::complex<double> X = (theory::W (k - 100.0) - theory::W (k + 100.0)
                                          + theory::W (k - 104.0) - theory::W (k + 104.0))
                                           / std::complex<double> (0.0, 2.0);
            return theory::smooth1 (theory::binDb (std::abs (X)));
        };
        for (int k : { 100, 102, 104 })
        {
            char label[80]; std::snprintf (label, sizeof (label), "codex4: two-tone complex linearity at bin %d", k);
            check (nearEq (probeBin (p, fs, (double) k * fs / theory::N).fill, twoToneDb (k), 0.08), label);
        }
    }

    //==========================================================================================
    // [codex #5] Peak-hold vs the tone returning MID-DECAY: run the documented recurrences in
    // double (theory), compare both fill and peak. L=80 -> the held peak must win; L=120 -> the
    // refilled smoothing must win. Off-by-one in the starve threshold or a swapped update order
    // shifts these by whole dB.
    for (int L : { 80, 120 })
    {
        Pane p; feedSine (p, 100.0, 40);
        for (int i = 0; i < L; ++i) p.starve();
        feedSine (p, 100.0, 1);

        const double d = theory::binDb (theory::sineBinMag (100.0, 100));
        double s = -120.0, pk = -120.0;
        for (int t = 0; t < 40; ++t) { s += 0.25 * (d - s); pk = std::max (pk - 0.8, s); }
        for (int i = 1; i <= L; ++i)
        {
            if (i >= 16) s += 0.05 * (-120.0 - s);                     // fade starts on the 16th tick
            pk = std::max (-120.0, pk - 0.8);                          // peak ALWAYS decays while starved
        }
        s += 0.25 * (d - s); pk = std::max (pk - 0.8, s);              // the returning frame

        const auto got = probeBin (p, fs, 100.0 * fs / theory::N);
        char l1[64], l2[64];
        std::snprintf (l1, sizeof (l1), "codex5: fill after return (L=%d)", L);
        std::snprintf (l2, sizeof (l2), "codex5: peak after return (L=%d, %s wins)", L, L == 80 ? "peak" : "fill");
        check (nearEq (got.fill, s, 0.10), l1);
        check (nearEq (got.peak, pk, 0.10), l2);
    }

    //==========================================================================================
    // [codex #6] Tilt algebra at the axis extremes on a flat (silent) spectrum — negative
    // octaves at 20 Hz must DROP the display, positive at 20 kHz must LIFT it, exactly
    // d(f) = -120 + 4.5·log2(f/1000), on a 1 dB/px map (y = 40 - d).
    {
        Pane p;                                                        // silent: flat -120 dB
        eqview::PlotMap pm; pm.width = 900.0f; pm.height = 240.0f; pm.plotBottom = 240.0f;
        pm.freqMin = 20.0; pm.freqMax = 20000.0; pm.specTop = 40.0; pm.specBottom = -200.0;
        float yFirst = 0.0f, yLast = 0.0f;
        p.buildColumns (pm, fs, 4.5, 1000.0, [&] (int i, float, float yF, float)
        { if (i == 0) yFirst = yF; yLast = yF; });
        check (nearEq (yFirst, 185.397352854, 0.01), "codex6: tilt at 20 Hz (hand: -120+4.5*log2(0.02))");
        check (nearEq (yLast,  140.551323573, 0.01), "codex6: tilt at 20 kHz (hand: -120+4.5*log2(20))");
    }

    //==========================================================================================
    // [codex #7, public-API variant] First column is a zero-width POINT at freqMin, interpolated —
    // never a peak-scan from DC. A DC-heavy signal (bins 0..1 hot, everything else silent) must
    // NOT leak into a first column parked at bin 5.12: interpolation reads the silent bins 5/6,
    // a wrong prevBin=0 peak-scan would read the ~0 dB bin 1 — a >25 dB separation.
    // HISTORY: this case REFUTED the verbatim-moved code (the inherited prevBin=0 seed) — the fix
    // seeds prevBin from the first column's own bin. Kept as the permanent regression pin.
    {
        Pane p;
        { float* in = p.frameInput(); for (int i = 0; i < theory::N; ++i) in[i] = 1.0f; p.ingest(); }
        eqview::PlotMap pm; pm.width = 256.0f; pm.height = 160.0f; pm.plotBottom = 160.0f;
        pm.freqMin = 120.0; pm.freqMax = 20000.0; pm.specTop = 20.0; pm.specBottom = -140.0;   // 1 dB/px
        float yFirst = -1.0f;
        p.buildColumns (pm, fs, 0.0, 1000.0, [&] (int i, float, float yF, float) { if (i == 0) yFirst = yF; });
        const double got = 20.0 - (double) yFirst;
        // theory: interp of bins 5..6 of a windowed DC — X[k] = W(k); one smooth tick from -120.
        const double bf = 120.0 * theory::N / fs;                      // 5.12
        const int b0 = (int) bf; const double t = bf - b0;
        const double d5 = theory::smooth1 (theory::binDb (std::abs (theory::W ((double) b0))));
        const double d6 = theory::smooth1 (theory::binDb (std::abs (theory::W ((double) b0 + 1))));
        const double expect = d5 + t * (d6 - d5);
        check (nearEq (got, expect, 1.5), "codex7: first column interpolates at freqMin (no DC peak-scan leak)");
        check (got < -80.0,               "codex7: first column stays >25 dB below the hot DC bins");
    }

    //==========================================================================================
    // [deepseek] Column bin coverage: a lone hot bin must NEVER be skipped between adjacent
    // columns — the peak branch consumes exactly (prevBin, curBin], the interp branch the
    // fractional inside — so for ANY hot bin the loudest column must carry it.
    for (double k : { 5.0, 37.0, 200.0, 511.0, 1023.0 })
    {
        Pane p; feedSine (p, k, 60);
        eqview::PlotMap pm; pm.width = 900.0f; pm.height = 160.0f; pm.plotBottom = 160.0f;
        pm.freqMin = 20.0; pm.freqMax = 28000.0; pm.specTop = 20.0; pm.specBottom = -140.0;
        float minY = 1.0e9f;
        p.buildColumns (pm, fs, 0.0, 1000.0, [&] (int, float, float yF, float) { minY = std::min (minY, yF); });
        const double got = 20.0 - (double) minY;                       // loudest column, dB
        const double d   = theory::binDb (theory::sineBinMag (k, (int) k));
        char label[80]; std::snprintf (label, sizeof (label), "deepseek: hot bin %.0f is never skipped by the columns", k);
        check (got > d - 0.7, label);
    }

    //==========================================================================================
    // [opus #1/#2] Converged absolute levels via a NARROW map (peak branch returns the loudest
    // bin exactly): half-bin scalloping -1.42645 dB, exact-bin -0.00424 dB (the symmetric-vs-
    // periodic Hann discriminator: periodic would read 0.000), and amplitude linearity 6.02060.
    {
        auto maxFillDb = [] (const Pane& p, double fsHz, double top, double bottom)
        {
            eqview::PlotMap pm; pm.width = 256.0f; pm.height = 96.0f; pm.plotBottom = 96.0f;
            pm.freqMin = 20.0; pm.freqMax = 28000.0; pm.specTop = top; pm.specBottom = bottom;
            float minY = 1.0e9f;
            p.buildColumns (pm, fsHz, 0.0, 1000.0, [&] (int, float, float yF, float) { minY = std::min (minY, yF); });
            return top - (top - bottom) / 96.0 * (double) minY;
        };
        Pane a; feedSine (a, 100.5, 60);
        check (nearEq (maxFillDb (a, fs, 6.0, -90.0), -1.42645, 0.05), "opus1: half-bin scalloping loss (converged)");
        Pane b; feedSine (b, 100.0, 60);
        const double dbHi = maxFillDb (b, fs, 6.0, -90.0);
        check (nearEq (dbHi, -0.00424, 0.003), "opus2: exact-bin absolute level (symmetric-window discriminator)");
        Pane c; feedSine (c, 100.0, 60, 0.5);
        const double dbLo = maxFillDb (c, fs, 6.0, -90.0);
        check (nearEq (dbHi - dbLo, 6.02060, 0.01), "opus2: amplitude linearity A vs A/2 == 6.0206 dB");

        // [opus #8] Above-Nyquist axis span at fs=8000 must be a flat floor plateau (clamp to the
        // Nyquist bin, NO folding of the 390.6 Hz tone upward) while the tone itself reads true.
        Pane d; feedSine (d, 100.0, 60);
        eqview::PlotMap deep; deep.width = 900.0f; deep.height = 96.0f; deep.plotBottom = 96.0f;
        deep.freqMin = 20.0; deep.freqMax = 28000.0; deep.specTop = 6.0; deep.specBottom = -186.0;
        double dbAt10k = 0.0, bestDx = 1e9; float minY = 1.0e9f;
        const double xT = deep.freqToX (10000.0);
        d.buildColumns (deep, 8000.0, 0.0, 1000.0, [&] (int, float x, float yF, float)
        {
            minY = std::min (minY, yF);
            const double dx = std::abs ((double) x - xT);
            if (dx < bestDx) { bestDx = dx; dbAt10k = 6.0 - 2.0 * (double) yF; }
        });
        check (dbAt10k <= -119.5, "opus8: above-Nyquist span is a -120 dB plateau (no folding)");
        check (nearEq (6.0 - 2.0 * (double) minY, -0.00424, 0.05), "opus8: the sub-Nyquist tone reads true at fs=8000");
    }

    //==========================================================================================
    // [opus #3, post-fix pin] First-column reading must be fs-CONSISTENT interpolation. Converged
    // windowed DC: specDb[0]=+6.0164, specDb[1]=+0.0021 (theory W(0),W(1)); at fs=192k the 20 Hz
    // column interpolates bins 0..1 at t=0.2133 -> 4.733 dB. At fs=8000 (bf=5.12) it interpolates
    // the SILENT bins 5..6 — the old prevBin=0 seed peak-scanned bins 1..5 and read ~0 dB here.
    {
        Pane p;
        { for (int t = 0; t < 60; ++t) { float* in = p.frameInput();
              for (int i = 0; i < theory::N; ++i) in[i] = 1.0f; p.ingest(); } }
        auto firstColDb = [&] (double fsHz)
        {
            eqview::PlotMap pm; pm.width = 256.0f; pm.height = 96.0f; pm.plotBottom = 96.0f;
            pm.freqMin = 20.0; pm.freqMax = 28000.0; pm.specTop = 12.0; pm.specBottom = -180.0;   // 2 dB/px
            float y0 = 0.0f;
            p.buildColumns (pm, fsHz, 0.0, 1000.0, [&] (int i, float, float yF, float) { if (i == 0) y0 = yF; });
            return 12.0 - 2.0 * (double) y0;
        };
        const double d0 = theory::binDb (std::abs (theory::W (0.0)));
        const double d1 = theory::binDb (std::abs (theory::W (1.0)));
        const double t192 = 20.0 * theory::N / 192000.0;               // 0.2133
        check (nearEq (firstColDb (192000.0), d0 + t192 * (d1 - d0), 0.1),
               "opus3: first column at fs=192k interpolates bins 0..1 (DC included)");
        const double bf8 = 20.0 * theory::N / 8000.0;                  // 5.12
        const double d5 = theory::binDb (std::abs (theory::W (5.0)));
        const double d6 = theory::binDb (std::abs (theory::W (6.0)));
        const double e8 = d5 + (bf8 - 5.0) * (d6 - d5);
        check (nearEq (firstColDb (8000.0), e8, 1.5),
               "opus3: first column at fs=8k interpolates bins 5..6 (regression pin for the prevBin bug)");
        check (firstColDb (8000.0) < -60.0,
               "opus3: no ~0 dB DC leak into the 20 Hz edge at fs=8k");
    }

    //==========================================================================================
    // [opus #4] Peak-hold vs smoothing branch contest when a QUIETER tone returns: the flip from
    // "decay wins" to "smoothing wins" lands on exactly the 7th quiet ingest (recurrences in
    // double; a drifted 0.25/0.8 constant or a swapped max() moves the flip).
    {
        Pane p; feedSine (p, 64.0, 60);
        auto maxPeakDb = [] (const Pane& q, double fsHz)
        {
            eqview::PlotMap pm; pm.width = 256.0f; pm.height = 96.0f; pm.plotBottom = 96.0f;
            pm.freqMin = 20.0; pm.freqMax = 28000.0; pm.specTop = 6.0; pm.specBottom = -90.0;
            float minY = 1.0e9f;
            q.buildColumns (pm, fsHz, 0.0, 1000.0, [&] (int, float, float, float yP) { minY = std::min (minY, yP); });
            return 6.0 - (double) minY;
        };
        const double dHi = theory::binDb (theory::sineBinMag (64.0, 64));
        const double dLo = theory::binDb (0.5 * theory::sineBinMag (64.0, 64));
        double sm = dHi, pk = dHi;
        for (int n = 1; n <= 7; ++n)
        {
            feedSine (p, 64.0, 1, 0.5);
            sm += 0.25 * (dLo - sm); pk = std::max (pk - 0.8, sm);
            if (n == 6) check (nearEq (maxPeakDb (p, fs), pk, 0.05), "opus4: n=6 — the 0.8/tick decay still wins");
            if (n == 7) check (nearEq (maxPeakDb (p, fs), pk, 0.05), "opus4: n=7 — smoothing takes over (exact flip tick)");
        }
    }

    //==========================================================================================
    // [opus #5] Tilt on the NEGATIVE-octave side: at 3 dB/oct the 20 Hz column must read exactly
    // 3*log2(0.02) = -16.932 dB BELOW the pivot (a sign flip would read above; log10/ln misuse
    // reads -7.36/-10.86).
    {
        Pane p;                                                        // silent: -120 dB flat
        eqview::PlotMap deep; deep.width = 900.0f; deep.height = 96.0f; deep.plotBottom = 96.0f;
        deep.freqMin = 20.0; deep.freqMax = 28000.0; deep.specTop = 6.0; deep.specBottom = -186.0;
        double db20 = 0.0, dbPivot = 0.0, b20 = 1e9, bPv = 1e9;
        const double xPv = deep.freqToX (1000.0);
        p.buildColumns (deep, fs, 3.0, 1000.0, [&] (int i, float x, float yF, float)
        {
            if (i == 0) db20 = 6.0 - 2.0 * (double) yF;
            const double dx = std::abs ((double) x - xPv);
            if (dx < bPv) { bPv = dx; dbPivot = 6.0 - 2.0 * (double) yF; }
        });
        check (nearEq (dbPivot, -120.0, 0.1), "opus5: tilt is zero at the pivot");
        check (nearEq (db20 - dbPivot, -16.93157, 0.1), "opus5: 20 Hz sits exactly 3*log2(0.02) dB BELOW the pivot");
    }

    //==========================================================================================
    // [opus #6] Clamped-region compositions saturate to the ENDPOINTS (non-invertible by design —
    // a removed clamp would extrapolate). [opus #7] dbToY reflection identity for ALL magnitudes.
    {
        eqview::PlotMap m; m.width = 1237.0f; m.height = 741.0f; m.plotBottom = 657.0f;
        m.freqMin = 20.0; m.freqMax = 28000.0; m.dbRange = 12.0;
        check (nearEq (m.xToFreq (m.freqToX (10.0)), 20.0, 1e-9),              "opus6: roundtrip below the axis saturates to freqMin");
        check (nearEq (m.xToFreq (m.freqToX (50000.0)), 28000.0, 28000.0 * 1e-6), "opus6: roundtrip above the axis saturates to freqMax");
        check (nearEq (m.freqToX (m.xToFreq (-100.0f)), 0.0, 1e-9),            "opus6: negative-x roundtrip saturates to 0");
        check (nearEq (m.freqToX (m.xToFreq (1.5f * m.width)), m.width, 1e-3), "opus6: past-width roundtrip saturates to width");
        for (double a : { 0.0, 5.0, 12.0, 24.0, 1000.0 })
        {
            char label[80]; std::snprintf (label, sizeof (label), "opus7: dbToY(a)+dbToY(-a) == plotBottom (a=%g)", a);
            check (nearEq ((double) m.dbToY (a) + (double) m.dbToY (-a), 657.0, 1e-3), label);
        }
        check (nearEq ((double) m.dbToY (1.0e6) + (double) m.dbToY (-1.0e6), 657.0, 5.0), "opus7: reflection holds even at 1e6 dB (unclamped, float ulp)");
    }

    std::printf (failures == 0 ? "eqview-refute: all checks passed\n" : "eqview-refute: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
