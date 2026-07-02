// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_events/juce_events.h>                    // MessageManager (callAsync), Thread
#include <juce_data_structures/juce_data_structures.h>  // PropertiesFile (pulls juce_core: URL, WebInputStream, JSON)
#include "AppPreferences.h"                             // the shared global PropertiesFile owner
#include <functional>

//==============================================================================
// tabby::UpdateChecker — opt-in update check, mirrored from orbitcab. Queries GitHub Releases ONLY
// when checkNow() is called from a user click (the (i) popover's "Check for updates" button) —
// NEVER on launch, NEVER silently. The plugin reads the latest release tag and decides "outdated"
// itself (see UpdateCompare.h), then persists the newer tag in a global PropertiesFile so the
// "update available" badge on the (i) button survives restarts and is shared across all instances.
//
// Contract:
//   GET https://api.github.com/repos/darwinscat/tabby-eq/releases/latest
//   200 -> { tag_name: "vX.Y.Z", html_url, name, … } — we read tag_name (strip 'v'), compare it to
//          the running build's kDescribe (UpdateCompare), and expose html_url as the release page.
//   404 = no releases yet, 403 = rate-limited; any non-200 / timeout / no network
//   => silent failure (ok=false) — this is an opt-in, non-blocking check.
//
// Adapter layer (host glue: network + storage). The teq:: DSP core is untouched. RT rule: the
// network call runs on a short-lived background thread — never on the audio thread.
//==============================================================================
namespace tabby
{

class UpdateChecker
{
public:
    // currentDescribe = TabbyVersion.h kDescribe (git-describe stamp), e.g. "v0.1.0" or "v0.1.0-78-g…".
    UpdateChecker (juce::String currentDescribe, AppPreferences& prefs);

    struct Result
    {
        bool         ok       = false;   // got a usable 200 response with a parseable tag
        bool         outdated = false;   // a newer release exists than the running build
        juce::String latest;             // the remote tag, 'v' stripped (e.g. "0.2.0")
        juce::String url;                // the release page (where to send the user)
        juce::String notes;              // optional human note (release title; empty if absent)
    };

    juce::String currentVersion() const { return current; }

    // Fire the check on a short-lived background thread; `onDone` is invoked on the message thread
    // with the result (ok=false on any failure — silent, opt-in).
    void checkNow (std::function<void (Result)> onDone);

    // Badge state: a stored "latest" tag that is newer than the running build.
    bool         updateAvailable() const;
    juce::String storedLatest()    const;   // the bare stored tag ('v' stripped), or empty

    // The canonical release-page URL (used when a stored tag exists but we have no saved html_url).
    static juce::String releasesPageUrl() { return "https://github.com/darwinscat/tabby-eq/releases/latest"; }

private:
    void storeOutdated (const juce::String& latest);
    void clearStored();
    static Result fetch (const juce::String& describe);         // blocking, no instance state — background thread only

    juce::String current;                                       // the running build's kDescribe
    AppPreferences& prefs;                                      // the shared global PropertiesFile owner

    // The network reply lands on the message thread via callAsync; if this checker (i.e. the plugin
    // instance) was destroyed meanwhile, the weak ref makes the callback a no-op.
    JUCE_DECLARE_WEAK_REFERENCEABLE (UpdateChecker)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};

} // namespace tabby
