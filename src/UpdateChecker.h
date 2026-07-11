// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <felitronics/appkit/UpdateChecker.h>

#include "AppPreferences.h"

//==============================================================================
// tabby::UpdateChecker — a thin ADAPTER over the family's shared checker (felitronics-appkit),
// which is this checker PROMOTED and consolidated with OrbitCab's copy, then crew-hardened. All
// policy (user-click only, silent failure, owned-thread join on destruction) and the whole GitHub
// flow live in the appkit header; this adapter only bakes in TabbyEQ's Config. The settings keys
// are appkit's defaults — identical to the historical TabbyEQ keys, so users' stored badges
// survive the swap byte-for-byte.
//==============================================================================
namespace tabby
{

struct UpdateChecker : felitronics::appkit::UpdateChecker
{
    // `currentDescribe` = tabby::currentDescribe() (the TabbyVersion.h git-describe stamp);
    // `prefs` must outlive this checker (the processor declares it first — see PluginProcessor.h).
    UpdateChecker (juce::String currentDescribe, AppPreferences& prefs)
        : felitronics::appkit::UpdateChecker ({ .ownerRepo      = "darwinscat/tabby-eq",
                                                .productName    = "TabbyEQ",
                                                .currentVersion = std::move (currentDescribe),
                                                .settings       = [&prefs] { return prefs.file(); } }) {}

    // The UI calls this statically (VersionInfo.cpp) — keep the static form the base can't offer.
    static juce::String releasesPageUrl() { return "https://github.com/darwinscat/tabby-eq/releases/latest"; }
};

} // namespace tabby
