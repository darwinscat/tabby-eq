// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_graphics/juce_graphics.h>

//==============================================================================
// tabby::chrome — the FabFilter-style top-toolbar "chrome" (brand blister + flat bar), reshaped
// IN PLACE into an extractable, product-agnostic shape ahead of its move to felitronics-appkit.
// Incubation discipline (mirrors src/eqview/): these headers take data/callbacks in and hand
// geometry out; the product (TabbyEQ) supplies the marks, models and colours. Nothing here draws a
// single pixel differently from the hand-rolled toolbar it replaces — the picture is verbatim.
namespace tabby::chrome
{

//==============================================================================
// ChromeMetrics — the ONE source of the bar geometry. `barHeight` is the flat toolbar band (the
// EQ bell's "0"); it drives BOTH the blister's bulge fill AND the shell's underline stroke, so the
// old triple-defined "30" (HeaderBrand::toolbarBottom / HeaderBrand::kLineY / the editor's kBarH)
// collapses to a single field. `blisterHeight` is the taller brand badge that overhangs the bar.
struct ChromeMetrics
{
    float barHeight     = 30.0f;   // flat toolbar band = the bell's "0" — feeds fill AND stroke
    float blisterHeight = 46.0f;   // the brand badge overhangs the bar by (blisterHeight - barHeight)
};

//==============================================================================
// ChromeTheme — the chrome's two paint colours, LOCAL to the EQ for now (the product seeds them
// from its palette; a shared appkit theme replaces this at the lib move). Kept a plain colour pair
// so the frame/overlay never reach into a product palette directly.
struct ChromeTheme
{
    juce::Colour fill;        // the blister bulge fill (the EQ seeds this from palette::bg())
    juce::Colour underline;   // the continuous toolbar-bottom hairline (white @ 0.07 alpha)
};

} // namespace tabby::chrome
