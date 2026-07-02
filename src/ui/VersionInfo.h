// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../UpdateChecker.h"

//==============================================================================
// The toolbar "(i)" button: a small icon button that pops a compact, panel-styled callout with the
// baked build/version stamp (describe · build number · hash+builder · core). Click the stamp block to
// copy it; the callout also carries an opt-in "Check for updates" affordance (the ONLY thing that hits
// the network — never on launch, never silent). When a newer release is known, a warm orange dot lights
// on the (i) button and persists across sessions (see tabby::UpdateChecker).
//
// The generated TabbyVersion.h is included ONLY by VersionInfo.cpp — since that header is rewritten
// on every build (a fresh build number), keeping its inclusion out of this widely-included header
// means only VersionInfo.cpp recompiles per build, not the whole editor.
namespace tabby
{
    // The running build's `git describe` stamp (TabbyVersion.h kDescribe), exposed as a function so
    // callers (the processor wiring the UpdateChecker) need not include the per-build generated header.
    const char* currentDescribe();

    class InfoButton : public juce::Button
    {
    public:
        explicit InfoButton (UpdateChecker& checker);
        void paintButton (juce::Graphics&, bool shouldDrawHighlighted, bool shouldDrawDown) override;

    private:
        void showInfoCallout();
        UpdateChecker& updateChecker;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InfoButton)
    };
}
