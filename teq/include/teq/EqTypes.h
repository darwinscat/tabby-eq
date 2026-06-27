// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <bit>
#include <cstdint>

namespace teq
{

// The maximum channel count the engine processes in one pass — the single source of truth for every
// per-channel state array (Svf, EqBand and EqEngine all derive their caps from this). 16 covers every
// commercial layout: mono, stereo, quad, 5.1, 7.1, 7.1.4 / 9.1.6 Atmos (≤ 16 channels) and 1st–3rd
// order ambisonics. The state is fixed-size and zero-allocation (so process() stays RT-safe), which
// means the only cost of the headroom is memory (~1.5 KB of biquad state per band at 16). Need more
// channels? Bump this one line — nothing else changes.
constexpr int kMaxChannels = 16;

enum class FilterType
{
    Bell,        // peaking, gainDb + Q
    LowShelf,    // gainDb (Butterworth slope, Q ignored)
    HighShelf,   // gainDb (Butterworth slope, Q ignored)
    HighPass,    // Q at 12 dB/oct; 24 dB/oct is Butterworth (Q ignored)
    LowPass,     // Q at 12 dB/oct; 24 dB/oct is Butterworth (Q ignored)
    BandPass,    // Q, unity gain at centre
    Notch,       // band-stop: deep null at f0, unity at DC/Nyquist, width by Q
    AllPass,     // flat magnitude, 360° phase rotation through f0 (Q = sharpness)
    Tilt         // spectral tilt about f0: lows -gainDb, highs +gainDb
};

// One band's full specification. The plugin adapter maps APVTS params into this; the engine
// owns no parameter system of its own (stays framework-agnostic).
struct BandParams
{
    bool       on     = false;
    FilterType type   = FilterType::Bell;
    double     freq   = 1000.0;   // Hz (engine clamps to [10, 0.49*fs])
    double     Q      = 1.0;
    double     gainDb = 0.0;      // bells & shelves
    int        slope  = 12;       // HP/LP only: 12 (uses Q) or 24 dB/oct (Butterworth, Q ignored)
    bool       swept  = false;    // true → zero-delay SVF (smooth fast fc sweeps for search mode)
    bool       bypass = false;    // band kept but muted (ghost) — distinct from on=false (removed)

    // Exact change-detection. The doubles are compared by bit pattern (not `==`) so the engine's
    // recompute-skip stays exact without tripping -Wfloat-equal in strict-warning builds.
    bool operator== (const BandParams& o) const noexcept
    {
        auto bits = [] (double d) noexcept { return std::bit_cast<std::uint64_t> (d); };
        return on == o.on && type == o.type && slope == o.slope && swept == o.swept && bypass == o.bypass
            && bits (freq) == bits (o.freq) && bits (Q) == bits (o.Q) && bits (gainDb) == bits (o.gainDb);
    }
};

} // namespace teq
