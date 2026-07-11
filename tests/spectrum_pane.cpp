// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.
//
// JUCE-free unit for the eqview SpectrumPane (src/eqview/SpectrumPane.h) — the analyzer pipeline
// (Hann window -> real FFT -> per-bin dB smoothing + peak-hold) and the "liquid" column sampler
// (interpolate where a column spans < 1 bin, bin-PEAK where it spans several, + display tilt).
// Pins the Hann single-bin compensation (a full-scale exact-bin sine reads ~0 dB), the peak-hold
// decay rate, the hold-then-fade starve behaviour, and the column/tilt geometry, so moving the
// pipeline out of the display can never silently change the analyzer's look.

#include "eqview/SpectrumPane.h"

#include <cmath>
#include <cstdio>

static int failures = 0;

static void check (bool ok, const char* what)
{
    if (! ok) { std::printf ("FAIL: %s\n", what); ++failures; }
    else        std::printf ("ok:   %s\n", what);
}

static bool nearEq (double a, double b, double tol) { return std::abs (a - b) <= tol; }

// Fill the pane's input with a unit sine at an exact bin centre and ingest it `ticks` times
// (the 0.25 smoothing converges geometrically — ~40 ticks is < 0.01 dB from the target).
static void feedSine (eqview::SpectrumPane& p, int bin, int ticks)
{
    constexpr double pi = 3.14159265358979323846;
    for (int t = 0; t < ticks; ++t)
    {
        float* in = p.frameInput();
        for (int i = 0; i < eqview::SpectrumPane::fftSize; ++i)
            in[i] = (float) std::sin (2.0 * pi * (double) bin * (double) i / (double) eqview::SpectrumPane::fftSize);
        p.ingest();
    }
}

int main()
{
    using Pane = eqview::SpectrumPane;

    // --- Hann single-bin compensation: a full-scale sine at an exact bin centre reads ~0 dB -----
    {
        Pane p;
        feedSine (p, 100, 60);
        // probe via buildColumns on a map whose spectrum scale we can invert: y = t*height
        eqview::PlotMap pm; pm.width = 900.0f; pm.height = 96.0f; pm.plotBottom = 96.0f;
        pm.specTop = 6.0; pm.specBottom = -90.0;   // 1 px per dB: y = (6 - dB)
        const double fs = 48000.0;
        const double fBin = 100.0 * fs / (double) Pane::fftSize;   // the sine's frequency
        float yAtSine = -1.0f; double xTarget = pm.freqToX (fBin);
        double bestDx = 1.0e9;
        p.buildColumns (pm, fs, 0.0, 1000.0, [&] (int, float x, float yFill, float)
        {
            const double dx = std::abs ((double) x - xTarget);
            if (dx < bestDx) { bestDx = dx; yAtSine = yFill; }
        });
        const double dbAtSine = 6.0 - (double) yAtSine;            // invert the 1px/dB map
        check (nearEq (dbAtSine, 0.0, 1.0), "full-scale exact-bin sine reads ~0 dB (Hann comp)");
    }

    // --- peak-hold: falls by exactly peakFallDb per starve tick until it meets the floor --------
    {
        Pane p;
        p.peakFallDb = 0.8f;
        feedSine (p, 64, 60);
        eqview::PlotMap pm; pm.width = 900.0f; pm.height = 96.0f; pm.plotBottom = 96.0f;
        pm.specTop = 6.0; pm.specBottom = -90.0;
        const double fs = 48000.0;
        auto peakDbAt = [&] (double fHz)
        {
            float best = 0.0f; double bestDx = 1.0e9; const double xT = pm.freqToX (fHz);
            p.buildColumns (pm, fs, 0.0, 1000.0, [&] (int, float x, float, float yPeak)
            { const double dx = std::abs ((double) x - xT); if (dx < bestDx) { bestDx = dx; best = yPeak; } });
            return 6.0 - (double) best;
        };
        const double fBin = 64.0 * fs / (double) Pane::fftSize;
        const double before = peakDbAt (fBin);
        p.starve(); p.starve(); p.starve();
        const double after = peakDbAt (fBin);
        check (nearEq (before - after, 3 * 0.8, 0.05), "peak-hold falls peakFallDb per starve tick");
    }

    // --- starve: HOLDS the fill spectrum for 15 ticks, only fades once genuinely starved --------
    {
        Pane p;
        feedSine (p, 64, 60);
        eqview::PlotMap pm; pm.width = 900.0f; pm.height = 96.0f; pm.plotBottom = 96.0f;
        pm.specTop = 6.0; pm.specBottom = -90.0;
        const double fs = 48000.0;
        const double fBin = 64.0 * fs / (double) Pane::fftSize;
        auto fillDbAt = [&] (double fHz)
        {
            float best = 0.0f; double bestDx = 1.0e9; const double xT = pm.freqToX (fHz);
            p.buildColumns (pm, fs, 0.0, 1000.0, [&] (int, float x, float yFill, float)
            { const double dx = std::abs ((double) x - xT); if (dx < bestDx) { bestDx = dx; best = yFill; } });
            return 6.0 - (double) best;
        };
        const double held = fillDbAt (fBin);
        for (int i = 0; i < 15; ++i) p.starve();                   // within the hold window
        check (nearEq (fillDbAt (fBin), held, 1.0e-6), "starve holds the spectrum for 15 ticks");
        // THEORY PIN: the fade is v <- v + 0.05*(-120 - v) = 0.95*v - 6, applied for the FIRST
        // time on exactly the 16th starve tick. The fade is affine and column interpolation is
        // linear, so the probe transforms by the same map — the expected values are exact.
        p.starve();
        const double afterOne = 0.95 * held - 6.0;
        check (nearEq (fillDbAt (fBin), afterOne, 0.05), "fade starts EXACTLY on the 16th tick, rate 0.05");
        p.starve();
        check (nearEq (fillDbAt (fBin), 0.95 * afterOne - 6.0, 0.05), "fade continues at exactly 0.05 per tick");
        for (int i = 0; i < 28; ++i) p.starve();                   // genuinely starved -> fades on down
        check (fillDbAt (fBin) < held - 3.0, "genuine starvation fades toward the floor");
    }

    // --- columns: count, monotonic x, and the tilt pivot ----------------------------------------
    {
        Pane p;   // silent pane: flat specDb == -120 everywhere
        eqview::PlotMap pm; pm.width = 900.0f; pm.height = 96.0f; pm.plotBottom = 96.0f;
        pm.specTop = 6.0; pm.specBottom = -90.0;
        int count = 0; float lastX = -1.0f; bool xMonotonic = true;
        p.buildColumns (pm, 48000.0, 0.0, 1000.0, [&] (int, float x, float, float)
        { if (x < lastX) xMonotonic = false; lastX = x; ++count; });
        check (count == 901, "900px plot emits 901 columns");
        check (xMonotonic && nearEq (lastX, pm.width, 1e-3), "columns ascend and end at width");

        // tilt: at the pivot the tilted and untilted values must agree; above it the tilt LIFTS
        // (higher dB -> smaller y). The silent pane sits at -120 dB, BELOW the display floor of the
        // default scale (everything would clamp to the bottom and hide the tilt) — so probe on a
        // deeper scale whose floor keeps -120 dB inside the plot: 6..-186 over 96 px = 2 dB/px.
        eqview::PlotMap deep = pm; deep.specBottom = -186.0;
        float yPivotNoTilt = 0, yPivotTilt = 0, yHiNoTilt = 0, yHiTilt = 0;
        const double xPivot = deep.freqToX (1000.0), xHi = deep.freqToX (8000.0);
        auto grab = [&] (double tilt, float& yPv, float& yHi8k)
        {
            double bp = 1e9, bh = 1e9;
            p.buildColumns (deep, 48000.0, tilt, 1000.0, [&] (int, float x, float yFill, float)
            {
                if (std::abs (x - xPivot) < bp) { bp = std::abs (x - xPivot); yPv  = yFill; }
                if (std::abs (x - xHi)    < bh) { bh = std::abs (x - xHi);    yHi8k = yFill; }
            });
        };
        grab (0.0, yPivotNoTilt, yHiNoTilt);
        grab (4.5, yPivotTilt,   yHiTilt);
        check (nearEq (yPivotTilt, yPivotNoTilt, 0.75), "tilt is anchored at the pivot");
        check (yHiTilt < yHiNoTilt - 5.0f, "tilt lifts the highs (+4.5 dB/oct at 8 kHz = +13.5 dB = 6.75 px here)");
    }

    // --- liquid sampling: a lone hot bin in the dense highs survives via the bin-PEAK rule ------
    {
        Pane p;
        feedSine (p, 800, 60);   // 800th bin @48k ≈ 18.75 kHz — many bins per column up there
        eqview::PlotMap pm; pm.width = 300.0f; pm.height = 96.0f; pm.plotBottom = 96.0f;   // narrow plot -> wide columns
        pm.specTop = 6.0; pm.specBottom = -90.0;
        float minY = 1.0e9f;
        p.buildColumns (pm, 48000.0, 0.0, 1000.0, [&] (int, float, float yFill, float)
        { minY = std::min (minY, yFill); });
        check (6.0 - (double) minY > -3.0, "hot bin survives wide columns (peak-detail rule)");
    }

    // --- crew pins (codex round): the constants that define the LOOK must not drift silently ----
    {
        // deep probe scale: 6..-186 dBFS over 96 px = 2 dB/px -> dB = 6 - 2*y (keeps -120 visible)
        eqview::PlotMap deep; deep.width = 900.0f; deep.height = 96.0f; deep.plotBottom = 96.0f;
        deep.specTop = 6.0; deep.specBottom = -186.0;
        auto fillDbAt = [&] (Pane& p, double fs, double fHz)
        {
            float best = 0.0f; double bestDx = 1.0e9; const double xT = deep.freqToX (fHz);
            p.buildColumns (deep, fs, 0.0, 1000.0, [&] (int, float x, float yFill, float)
            { const double dx = std::abs ((double) x - xT); if (dx < bestDx) { bestDx = dx; best = yFill; } });
            return 6.0 - 2.0 * (double) best;
        };

        // 0.25 smoothing: ONE ingest from the -120 floor toward a ~0 dB exact-bin sine lands at
        // ~-90 dB (-120 + 0.25*120). A changed smoothing constant moves this by tens of dB.
        { Pane p; feedSine (p, 100, 1);
          check (nearEq (fillDbAt (p, 48000.0, 100.0 * 48000.0 / Pane::fftSize), -90.0, 2.5), "0.25 smoothing: first tick lands at ~-90 dB"); }

        // zero input stays EXACTLY on the -120 floor (gain 0 -> minus-infinity clamp -> no drift)
        { Pane p; for (int t = 0; t < 3; ++t) { float* in = p.frameInput();
              for (int i = 0; i < Pane::fftSize; ++i) in[i] = 0.0f; p.ingest(); }
          check (nearEq (fillDbAt (p, 48000.0, 1000.0), -120.0, 1e-3), "zero input holds the -120 dB floor"); }

        // DC packing (spec[0], im forced 0): windowed DC reads ~+6 dB at bin 0 (Hann sum = N/2,
        // norm = N/4). Probe with a huge fs so the 20 Hz column sits at 98% of bin 0.
        { Pane p; for (int t = 0; t < 60; ++t) { float* in = p.frameInput();
              for (int i = 0; i < Pane::fftSize; ++i) in[i] = 1.0f; p.ingest(); }
          check (nearEq (fillDbAt (p, 2048000.0, 20.0), 6.0, 0.8), "DC bin packing reads ~+6 dB"); }

        // Nyquist packing (spec[1]): an alternating +-1 signal is a full-scale Nyquist tone
        // (+6 dB at bin N/2). The interpolation branch deliberately saturates at bin N/2-1 (its
        // bf clamp), so the Nyquist bin reaches the display ONLY through the bin-PEAK branch of
        // wide top columns — pin exactly that: with freqMax past fs/2 the loudest column must
        // carry the +6 dB through the spec[1] slot (a broken special case reads ~0 dB leakage).
        { Pane p; for (int t = 0; t < 60; ++t) { float* in = p.frameInput();
              for (int i = 0; i < Pane::fftSize; ++i) in[i] = (i & 1) ? -1.0f : 1.0f; p.ingest(); }
          float minY = 1.0e9f;
          p.buildColumns (deep, 40000.0, 0.0, 1000.0, [&] (int, float, float yFill, float)
          { minY = std::min (minY, yFill); });
          check (nearEq (6.0 - 2.0 * (double) minY, 6.0, 0.8), "Nyquist bin packing contributes through spec[1]"); }

        // N clamp bounds: tiny plot still gets 256 columns, huge plot caps at 900.
        { Pane p; int c1 = 0, c2 = 0;
          eqview::PlotMap tiny = deep;  tiny.width = 100.0f;
          eqview::PlotMap wide = deep;  wide.width = 2000.0f;
          p.buildColumns (tiny, 48000.0, 0.0, 1000.0, [&] (int, float, float, float) { ++c1; });
          p.buildColumns (wide, 48000.0, 0.0, 1000.0, [&] (int, float, float, float) { ++c2; });
          check (c1 == 257 && c2 == 901, "column count clamps: 100px -> 257, 2000px -> 901"); }

        // interpolation INSIDE one bin: at low frequencies consecutive columns share a bin pair yet
        // move smoothly (nearest-bin sampling would stair-step to equal values).
        { Pane p; feedSine (p, 3, 60);
          const double fs = 48000.0, binPerHz = (double) Pane::fftSize / fs;
          bool smoothPair = false; double prevF = -1.0; float prevY = 0.0f;
          p.buildColumns (deep, fs, 0.0, 1000.0, [&] (int, float x, float yFill, float)
          {
              const double f = deep.xToFreq (x);
              if (prevF > 0.0 && (int) std::floor (prevF * binPerHz) == (int) std::floor (f * binPerHz)
                  && std::abs (yFill - prevY) > 1.0e-4f)
                  smoothPair = true;
              prevF = f; prevY = yFill;
          });
          check (smoothPair, "columns interpolate INSIDE a bin (liquid lows, no stair-steps)"); }
    }

    std::printf (failures == 0 ? "SpectrumPane: all checks passed\n" : "SpectrumPane: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
