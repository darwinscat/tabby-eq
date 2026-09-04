// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <felitronics/eqview/TraceSet.h>
#include <felitronics/eqview/HandleMath.h>
#include <felitronics/eqview/PlotGrid.h>
#include <felitronics/eqview/PlotSurface.h>
#include <felitronics/analysis/PlotMap.h>

//==============================================================================
// The library's names, under the one this product has always used.
//
// The response-curve calculator, the handle geometry and the log ruler grew up in this repo's
// src/eqview/ incubator and GRADUATED to felitronics-eqview — its own repo, core-only dependencies,
// tests upstream. The coordinate map went further back, to felitronics-core, where the arithmetic
// belongs. This header pulls all of it back under `eqview::`, so the graduation cost src/ nothing:
// the product still says eqview::TraceSet, eqview::handles and eqview::grid, and the code behind
// those names is now something the rest of the family can use.
//
// (TabbyEQ's OWN eqview:: things — the analyzer PaneBox seam and its factories — are declared in
// ui/EqCurveDisplay.h, in this same namespace. A product may extend the vocabulary; it must not
// fork it.)
namespace eqview
{
    using felitronics::eqview::TraceSet;
    using felitronics::eqview::PlotSurface;
    namespace grid    = felitronics::eqview::grid;
    namespace handles = felitronics::eqview::handles;

    using felitronics::analysis::PlotMap;
}
