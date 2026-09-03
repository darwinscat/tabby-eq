// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>   // AudioProcessor::WrapperType (the running format)
#include <felitronics/appkit/VersionBadge.h>   // the family's shared version badge + its popover

#include "../UpdateChecker.h"

//==============================================================================
// The toolbar "(i)" button: a small icon button that casts the FAMILY version popover — felitronics-
// appkit's VersionBadge window (brand mark, GitHub-linked build stamp, click-the-stamp-to-copy, the
// opt-in "Check for updates" — the ONLY thing that hits the network, never on launch, never silent —
// and the Feed the Cat block). TabbyEQ used to carry its own copy of that window; it is gone, and the
// badge lives on invisibly beside this button as the config + checker holder (see makeVersionBadgeConfig).
// When a newer release is known, a warm orange dot lights on the (i) and persists across sessions.
//
// The generated TabbyVersion.h is included ONLY by VersionInfo.cpp — since that header is rewritten
// on every build (a fresh build number), keeping its inclusion out of this widely-included header
// means only VersionInfo.cpp recompiles per build, not the whole editor.
namespace tabby
{
    // The running build's `git describe` stamp (TabbyVersion.h kDescribe), exposed as a function so
    // callers (the processor wiring the UpdateChecker) need not include the per-build generated header.
    const char* currentDescribe();

    // The running format, spelled the way the window shows it (CLAP reports wrapperType_Undefined —
    // among the formats TabbyEQ ships that is unambiguously CLAP; mirrors OrbitCab's mapping).
    juce::String pluginFormatName (juce::AudioProcessor::WrapperType);

    // TabbyEQ's product config for the shared felitronics::appkit::VersionBadge — the family About
    // window (marks + GitHub-linked table + opt-in update check + the Feed the Cat block). Assembled
    // HERE so the per-build generated TabbyVersion.h stays included by VersionInfo.cpp alone.
    // `format` = pluginFormatName (proc.wrapperType): it decides one word of wording — running as
    // Standalone the thing in your hands is an app, everywhere else it is a plugin.
    felitronics::appkit::VersionBadge::Config makeVersionBadgeConfig (const juce::String& format);

    class InfoButton : public juce::Button
    {
    public:
        // `versionBadge` supplies the window; both it and the checker must outlive this button (the
        // editor declares them first).
        InfoButton (UpdateChecker& checker, felitronics::appkit::VersionBadge& versionBadge);
        void paintButton (juce::Graphics&, bool shouldDrawHighlighted, bool shouldDrawDown) override;

    private:
        UpdateChecker& updateChecker;                        // the orange "update available" dot
        felitronics::appkit::VersionBadge& badge;            // the window this button casts
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InfoButton)
    };
}
