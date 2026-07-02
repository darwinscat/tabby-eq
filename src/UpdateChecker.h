// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_events/juce_events.h>                    // AsyncUpdater (message-thread delivery)
#include <juce_data_structures/juce_data_structures.h>  // PropertiesFile (pulls juce_core: Thread, URL, WebInputStream, JSON)
#include "AppPreferences.h"                             // the shared global PropertiesFile owner
#include <functional>

//==============================================================================
// tabby::UpdateChecker — opt-in update check, mirrored from orbitcab. Queries GitHub Releases ONLY
// when checkNow() is called from a user click (the (i) popover's "Check for updates" button) —
// NEVER on launch, NEVER silently. The plugin reads the latest release tag and decides "outdated"
// itself (see UpdateCompare.h), then persists the newer tag in the global PropertiesFile so the
// "update available" badge on the (i) button survives restarts and is shared across instances.
//
// Contract:
//   GET https://api.github.com/repos/darwinscat/tabby-eq/releases/latest
//   200 -> { tag_name: "vX.Y.Z", html_url, name, … } — we read tag_name (strip 'v'), compare it to
//          the running build's kDescribe (UpdateCompare), and expose html_url as the release page.
//   404 = no releases yet, 403 = rate-limited; any non-200 / timeout / no network
//   => silent failure (ok=false) — this is an opt-in, non-blocking check.
//
// Threading (deliberate deviation from orbitcab, which fires a DETACHED juce::Thread::launch and
// reaches back via a cross-thread WeakReference — unsafe if the host unloads the plugin while the
// request stalls; noted upstream): the worker here is an OWNED juce::Thread base, so destruction
// JOINS it — signalThreadShouldExit() + WebInputStream::cancel() abort a blocking connect/read
// promptly, then stopThread() waits. The result is handed to the message thread via AsyncUpdater
// (cancelPendingUpdate() in the destructor drops an undelivered one). Hosts destroy the processor
// on the message thread (JUCE wrapper guarantee), so a delivered callback can't race destruction.
//
// Adapter layer (host glue: network + storage). The teq:: DSP core is untouched. RT rule: the
// network call runs on the owned background thread — never on the audio thread.
//==============================================================================
namespace tabby
{

class UpdateChecker : private juce::Thread,
                      private juce::AsyncUpdater
{
public:
    // currentDescribe = TabbyVersion.h kDescribe (git-describe stamp), e.g. "v0.1.0" or "v0.1.0-78-g…".
    UpdateChecker (juce::String currentDescribe, AppPreferences& prefs);
    ~UpdateChecker() override;

    struct Result
    {
        bool         ok       = false;   // got a usable 200 response with a parseable tag
        bool         outdated = false;   // a newer release exists than the running build
        juce::String latest;             // the remote tag, 'v' stripped (e.g. "0.2.0")
        juce::String url;                // the release page (where to send the user)
        juce::String notes;              // optional human note (release title; empty if absent)
    };

    juce::String currentVersion() const { return current; }

    // Fire the check on the owned background worker; `onDone` is invoked on the message thread with
    // the result (ok=false on any failure — silent, opt-in). MESSAGE THREAD ONLY. If a check is
    // already in flight, the new callback replaces the pending one (last click wins) — the in-flight
    // result is delivered to it.
    void checkNow (std::function<void (Result)> onDone);

    // Badge state: a stored "latest" tag that is newer than the running build.
    bool         updateAvailable() const;
    juce::String storedLatest()    const;   // the bare stored tag ('v' stripped), or empty

    // The canonical release-page URL (used when a stored tag exists but we have no saved html_url).
    static juce::String releasesPageUrl() { return "https://github.com/darwinscat/tabby-eq/releases/latest"; }

private:
    void run() override;                // worker: the blocking fetch, then triggerAsyncUpdate
    void handleAsyncUpdate() override;  // message thread: persist + notify
    Result fetch();                     // worker-only: registers the cancellable stream while blocking

    void storeOutdated (const juce::String& latest);
    void clearStored();

    juce::String current;                                       // the running build's kDescribe
    AppPreferences& prefs;                                      // the shared global PropertiesFile owner

    std::function<void (Result)> onDone;                        // message thread only (checkNow / handleAsyncUpdate)
    Result pending;                                             // written by the worker before triggerAsyncUpdate

    // The in-flight stream, registered so the destructor can abort a blocking connect/read from
    // another thread (WebInputStream::cancel is documented cross-thread-safe). Guarded by streamLock:
    // the worker only publishes/retires the pointer under the lock, so cancel() can never race the
    // stream's stack-scope destruction.
    juce::CriticalSection streamLock;
    juce::WebInputStream* activeStream = nullptr;               // guarded by streamLock

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UpdateChecker)
};

} // namespace tabby
