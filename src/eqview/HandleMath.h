// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <teq/EqTypes.h>

#include "PlotMap.h"

#include <algorithm>
#include <cmath>

//==============================================================================
// HandleMath — the JUCE-free node/whisker geometry of the EQ view (eqview layer, step 0
// incubation): the pure functions that place a filter's node and its Q/slope "whisker" handles on
// the plot, and classify a filter TYPE by how it behaves under a drag (has a gain node? rests on
// 0 dB? Q- vs slope-driven whiskers?). No component state, no JUCE — takes a filter type / Q /
// slope + a PlotMap and returns positions and grab tests. The GESTURE state machine (which handle
// is being dragged, long-press-to-solo, add-drag) stays with the owning component for now.
namespace eqview::handles
{

//==============================================================================
// Filter-type behaviour under editing (core FilterType -> how the node/whiskers read).
inline bool hasGain (teq::FilterType t) noexcept
{
    return t == teq::FilterType::Bell     || t == teq::FilterType::LowShelf
        || t == teq::FilterType::HighShelf || t == teq::FilterType::Tilt;
}

inline bool isCut (teq::FilterType t) noexcept
{
    return t == teq::FilterType::HighPass || t == teq::FilterType::LowPass;
}

inline bool qRelevant (teq::FilterType t) noexcept   // Q sets bandwidth -> show the Q whiskers
{                                                    // HP/LP use discrete slopes (their own whisker), not Q
    return t == teq::FilterType::Bell      || t == teq::FilterType::BandPass
        || t == teq::FilterType::Notch     || t == teq::FilterType::AllPass
        || t == teq::FilterType::LowShelf  || t == teq::FilterType::HighShelf;   // resonant shelves have Q
}

// Whiskers that encode the discrete SLOPE (octaves) via the Neutron-style handle spread, not a
// continuous Q: HP/LP, and — since core gave them a variable order — the Notch and BandPass.
inline bool slopeWhisker (teq::FilterType t) noexcept
{
    return isCut (t) || t == teq::FilterType::Notch || t == teq::FilterType::BandPass;
}

inline bool whiskerRelevant (teq::FilterType t) noexcept { return qRelevant (t) || isCut (t); }

// A node RESTS ON 0 dB (no vertical drag) for the gain-less surgical/phase types — cuts, notch,
// all-pass. hasGain types ride their own gain; everything else (only BandPass) rides the composite
// at its corner. Single home for what nodePos and the add-preview ghost must agree on.
inline bool restsOnZeroDb (teq::FilterType t) noexcept
{
    return isCut (t) || t == teq::FilterType::Notch || t == teq::FilterType::AllPass;
}

//==============================================================================
// Q-whisker calibration — half-bandwidth (octaves) <-> Q, log-linear. Tuned by feel so the handles
// span a usable range: 0.2 oct out = max Q 40, 0.833 oct out = min Q 0.1.
inline constexpr double kQwBwLo = 0.2, kQwBwHi = 0.833, kQwQHi = 40.0, kQwQLo = 0.1;

inline double whiskerBwForQ (double Q) noexcept
{
    const double t = (std::log10 (kQwQHi) - std::log10 (std::clamp (Q, kQwQLo, kQwQHi)))
                   / (std::log10 (kQwQHi) - std::log10 (kQwQLo));
    return kQwBwLo + t * (kQwBwHi - kQwBwLo);
}

inline double whiskerQForBw (double bw) noexcept
{
    const double t = std::clamp ((bw - kQwBwLo) / (kQwBwHi - kQwBwLo), 0.0, 1.0);
    return std::pow (10.0, std::log10 (kQwQHi) - t * (std::log10 (kQwQHi) - std::log10 (kQwQLo)));
}

//==============================================================================
// HP/LP/Notch/BP whisker maps to the DISCRETE slope list (steeper = narrower handle) instead of Q.
inline constexpr int kSlopeDb[7] = { 6, 12, 24, 36, 48, 72, 96 };

inline int slopeIndexFromDb (int db) noexcept { for (int i = 0; i < 7; ++i) if (kSlopeDb[i] == db) return i; return 1; }
inline double slopeBwForIndex (int i) noexcept { return kQwBwHi + (kQwBwLo - kQwBwHi) * (i / 6.0); }   // i0 wide -> i6 narrow
inline int slopeIndexForBw (double bw) noexcept
{
    const double t = std::clamp ((kQwBwHi - bw) / (kQwBwHi - kQwBwLo), 0.0, 1.0);
    return std::clamp ((int) std::round (t * 6.0), 0, 6);
}

// The half-bandwidth (octaves) a node's whiskers should span for its type + Q/slope.
inline double whiskerBw (teq::FilterType t, double q, int slopeDb) noexcept
{
    return slopeWhisker (t) ? slopeBwForIndex (slopeIndexFromDb (slopeDb)) : whiskerBwForQ (q);
}

//==============================================================================
// Geometry over a PlotMap.
struct Pt { float x = 0.0f, y = 0.0f; };

// The drawn node radius (px) and the grab pads over it — the single home for the hit-test radii
// so the drawing, the node grab, the whisker-handle grab and the add-clearance can never drift
// apart on separate literals. (Pads: node = 5, whisker handle = 4, "+"-clearance = 16.)
inline constexpr float kNodeR = 6.0f;
inline constexpr float kNodeGrabPad = 5.0f, kWhiskerGrabPad = 4.0f, kAddClearPad = 16.0f;

// The two whisker handle endpoints: symmetric in LOG frequency about the node, at ± the
// half-bandwidth. Returns pixel points at the node's own Y.
struct WhiskerEnds { Pt left, right; };
inline WhiskerEnds whiskerEndsPx (const PlotMap& pm, Pt node, double f0, double bwOct) noexcept
{
    const float dx = pm.freqToX (f0 * std::exp2 (bwOct)) - node.x;   // log-x, symmetric
    return { { node.x - dx, node.y }, { node.x + dx, node.y } };
}

// Squared grab radius for a node/handle of drawn radius `nodeR` with `pad` extra reach.
inline float grabRadiusSq (float nodeR, float pad) noexcept { return (nodeR + pad) * (nodeR + pad); }

inline float distanceSq (Pt a, Pt b) noexcept { const float dx = a.x - b.x, dy = a.y - b.y; return dx * dx + dy * dy; }

} // namespace eqview::handles
