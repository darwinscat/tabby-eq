// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
// The toolbar "(i)" button: a small icon button that pops a compact, panel-styled callout with the
// baked build/version stamp (describe · build number · hash+builder · core). Click the callout to
// copy the whole block; it dismisses on outside-click / Esc (CallOutBox default).
//
// The generated TabbyVersion.h is included ONLY by VersionInfo.cpp — since that header is rewritten
// on every build (a fresh build number), keeping its inclusion out of this widely-included header
// means only VersionInfo.cpp recompiles per build, not the whole editor.
namespace tabby
{
    class InfoButton : public juce::Button
    {
    public:
        InfoButton();
        void paintButton (juce::Graphics&, bool shouldDrawHighlighted, bool shouldDrawDown) override;

    private:
        void showInfoCallout();
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InfoButton)
    };
}
