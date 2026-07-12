// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <felitronics/core/Fft.h>
#include <felitronics/analysis/SpectrumTap.h>   // canonical home of kSpectrumFftSize (not the teq shim —
                                                // the future lib move must not depend on compat targets)
#include "PlotMap.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

//==============================================================================
// SpectrumPane — the analyzer's JUCE-free pipeline + state (eqview layer, step 0 incubation).
// Pull-fed frames go Hann window → real FFT → per-bin dB with smoothing + a slow-decay peak-hold;
// buildColumns() then samples the bins into log-frequency plot columns — the "liquid" rule: where a
// column spans LESS than one bin, interpolate between neighbours (smooth lows), where it spans
// several, take the bin PEAK (narrow HF resonances stay visible) — applies the pink-noise display
// tilt and maps through PlotMap. The owner draws the emitted points (path building stays JUCE-side)
// and owns the frame SOURCE: fill frameInput() and call ingest(), or starve() when no frame arrived
// this tick (normal at 30 Hz vs ~23 fps frames — the pane holds, then fades once genuinely starved).
// Message-thread only; the audio thread is never touched (frames arrive via a lock-free tap).
namespace eqview
{

struct SpectrumPane
{
    static constexpr int fftSize = felitronics::analysis::kSpectrumFftSize;
    static constexpr int numBins = fftSize / 2 + 1;

    float peakFallDb = 0.8f;          // peak-hold decay per tick (~24 dB/s at 30 Hz)

    SpectrumPane()
    {
        constexpr float pi = 3.14159265358979323846f;
        for (int i = 0; i < fftSize; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * pi * (float) i / (float) (fftSize - 1));
        fft.prepare (fftSize);        // plan/alloc once, on the owner's (UI) thread — never per tick
        specDb.fill (-120.0f);
        specPeak.fill (-120.0f);
    }

    // Fill with fftSize samples (the tap frame), then call ingest(). PROTOCOL: exactly one
    // ingest() per fill — the window is applied IN PLACE, so ingesting the same buffer twice
    // would Hann² the data. The debug guard trips on the first violation.
    float* frameInput() noexcept { frameArmed = true; return fftBuf.data(); }

    void ingest() noexcept
    {
        assert (frameArmed && "SpectrumPane: fill frameInput() before every ingest()");
        frameArmed = false;
        starveTicks = 0;

        for (int i = 0; i < fftSize; ++i) fftBuf[(size_t) i] *= window[(size_t) i];
        fft.forward (fftBuf.data(), spec.data());                 // real[N] -> spectrum [DC, Nyquist, re1,im1, …]

        const double norm = (double) fftSize * 0.25;              // Hann single-bin compensation
        for (int i = 0; i < numBins; ++i)
        {
            const float re  = (i == 0) ? spec[0] : (i == fftSize / 2) ? spec[1] : spec[(size_t) (2 * i)];
            const float im  = (i == 0 || i == fftSize / 2) ? 0.0f : spec[(size_t) (2 * i + 1)];
            const float mag = std::sqrt (re * re + im * im);      // |bin| of the unnormalized forward
            const double g  = (double) mag / norm;                // == juce::Decibels::gainToDecibels (g, -120)
            const double db = g > 0.0 ? std::max (-120.0, 20.0 * std::log10 (g)) : -120.0;
            specDb[(size_t) i]  += 0.25f * ((float) db - specDb[(size_t) i]);   // smooth toward target
            specPeak[(size_t) i] = std::max (specPeak[(size_t) i] - peakFallDb, specDb[(size_t) i]);
        }
    }

    // No new frame this tick is NORMAL (a 2048-pt window arrives at ~23 fps vs the 30 Hz timer).
    // Hold the last spectrum; only fade once genuinely starved (audio stopped ~0.5 s).
    void starve() noexcept
    {
        if (starveTicks < 16) ++starveTicks;                      // bounded — never overflows over long silence
        if (starveTicks > 15)
            for (auto& v : specDb) v += 0.05f * (-120.0f - v);
        for (auto& v : specPeak) v = std::max (-120.0f, v - peakFallDb);
    }

    // Sample the bins into N+1 log-frequency columns and emit plot points through the map.
    // emit (int i, float x, float yFill, float yPeak) is called with i = 0..N, x ascending.
    template <class Emit>
    void buildColumns (const PlotMap& pm, double fs, double tiltDbPerOct, double tiltPivotHz, Emit&& emit) const
    {
        const double binPerHz = (double) fftSize / fs;
        const int    N = std::clamp ((int) pm.width, 256, 900);

        // The FIRST column is a zero-width point at freqMin — seed prevBin from ITS bin, not from
        // DC: a prevBin = 0 seed made column 0 peak-scan (0, curBin] and leak DC/bin-1 energy into
        // the left edge whenever freqMin sits above bin 1 (low sample rates, zoomed-in maps).
        // Found by the crew's theory suite (codex #7); on the classic 20 Hz axis at ≥44.1 kHz both
        // seeds are bin 0, so the fix is bit-identical there.
        int prevBin = std::clamp ((int) std::floor (pm.xToFreq (0.0f) * binPerHz), 0, numBins - 1);
        for (int i = 0; i <= N; ++i)
        {
            const float  x = (float) i / (float) N * pm.width;
            const double f = pm.xToFreq (x);
            const int    curBin = std::clamp ((int) std::floor (f * binPerHz), 0, numBins - 1);
            const float  tilt = (float) (tiltDbPerOct * std::log2 (f / tiltPivotHz));   // pink-noise comp
            emit (i, x, pm.specDbToY (column (specDb,   f, binPerHz, prevBin, curBin) + tilt),
                        pm.specDbToY (column (specPeak, f, binPerHz, prevBin, curBin) + tilt));
            prevBin = curBin;
        }
    }

private:
    template <class Arr>
    float column (const Arr& raw, double f, double binPerHz, int loBin, int hiBin) const noexcept
    {
        if (hiBin <= loBin)                                       // < 1 bin this column -> interpolate
        {
            const double bf = std::clamp (f * binPerHz, 0.0, (double) (numBins - 2));
            const int b0 = (int) bf; const float t = (float) (bf - (double) b0);
            return raw[(size_t) b0] + t * (raw[(size_t) (b0 + 1)] - raw[(size_t) b0]);
        }
        float m = -200.0f;                                        // >= 1 bin -> peak (max) = detail
        for (int b = std::max (0, loBin + 1); b <= std::min (numBins - 1, hiBin); ++b)
            m = std::max (m, raw[(size_t) b]);
        return m;
    }

    felitronics::core::fft::DefaultRealFft fft;
    std::array<float, fftSize>     window {};
    std::array<float, fftSize * 2> fftBuf {};
    std::array<float, fftSize>     spec {};
    std::array<float, numBins>     specDb {};
    std::array<float, numBins>     specPeak {};                   // slow-decay peak-hold
    int starveTicks = 0;                                          // consecutive ticks with no new frame
    bool frameArmed = false;                                      // debug protocol guard (see frameInput)
};

} // namespace eqview
