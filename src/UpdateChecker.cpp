// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "UpdateChecker.h"
#include "UpdateCompare.h"   // the pure, JUCE-free compare rule (also unit-tested off the plugin)

#include <utility>           // std::exchange

namespace tabby
{

namespace
{
    constexpr int  kTimeoutMs     = 5000;   // short — opt-in, non-blocking
    // Join budget on destruction: the connect timeout is 5 s and cancel() aborts a blocked
    // connect/read promptly, so 8 s is generous headroom before stopThread force-kills (last resort).
    constexpr int  kJoinMs        = 8000;
    constexpr char kReleasesApi[] = "https://api.github.com/repos/darwinscat/tabby-eq/releases/latest";
    constexpr char kKeyLatest[]   = "updateLastSeenLatest";
    constexpr char kKeyEpoch[]    = "updateLastCheckEpoch";

    // A remote tag ("v0.2.0"/"0.2.0") stored/compared bare: strip a single leading 'v'.
    juce::String stripV (juce::String tag)
    {
        tag = tag.trim();
        return tag.startsWithIgnoreCase ("v") ? tag.substring (1) : tag;
    }
}

UpdateChecker::UpdateChecker (juce::String currentDescribe, AppPreferences& prefsRef)
    : juce::Thread ("TabbyEQ UpdateChecker"),
      current (std::move (currentDescribe)), prefs (prefsRef)
{
    // Cross-process freshness: same-process instances share the PropertiesFile object (see
    // AppPreferences), but another HOST PROCESS may have stored a newer tag — pick it up once here.
    prefs.reload();

    // Badge-clear: if the running build has caught up to (or passed) a previously seen "latest",
    // drop the stored value so no stale badge shows. The ONLY place the plugin compares at ctor time.
    const juce::String seen = storedLatest();
    if (seen.isNotEmpty() && ! update::remoteIsNewer (current.toStdString(), seen.toStdString()))
        clearStored();
}

UpdateChecker::~UpdateChecker()
{
    // JOIN the worker before the plugin module can unload: signal, abort any blocking connect/read,
    // wait. Then drop an undelivered async result (the AsyncUpdater base asserts on pending updates).
    signalThreadShouldExit();
    {
        const juce::ScopedLock sl (streamLock);
        if (activeStream != nullptr)
            activeStream->cancel();                 // cross-thread-safe: unblocks connect/read promptly
    }
    stopThread (kJoinMs);
    cancelPendingUpdate();
}

juce::String UpdateChecker::storedLatest() const
{
    if (auto* s = prefs.file())
        return s->getValue (kKeyLatest);
    return {};
}

bool UpdateChecker::updateAvailable() const
{
    const juce::String seen = storedLatest();
    return seen.isNotEmpty() && update::remoteIsNewer (current.toStdString(), seen.toStdString());
}

void UpdateChecker::storeOutdated (const juce::String& latest)
{
    if (auto* s = prefs.file())
    {
        s->setValue (kKeyLatest, latest);
        s->setValue (kKeyEpoch, juce::String (juce::Time::getCurrentTime().toMilliseconds()));
        s->saveIfNeeded();
    }
}

void UpdateChecker::clearStored()
{
    if (auto* s = prefs.file())
    {
        s->removeValue (kKeyLatest);
        s->saveIfNeeded();
    }
}

// Worker thread. Blocking; the stream is registered under streamLock while it can block so the
// destructor can cancel() it from another thread.
UpdateChecker::Result UpdateChecker::fetch()
{
    Result r;

    // GitHub's API rejects requests without a User-Agent (403); Accept + api-version are best
    // practice. Unauthenticated → 60 req/h per IP, ample for an opt-in manual check.
    const juce::String headers =
        "User-Agent: TabbyEQ-UpdateChecker\r\n"
        "Accept: application/vnd.github+json\r\n"
        "X-GitHub-Api-Version: 2022-11-28";

    juce::WebInputStream web (juce::URL (kReleasesApi), false);
    web.withConnectionTimeout (kTimeoutMs)
       .withNumRedirectsToFollow (3)
       .withExtraHeaders (headers);

    {
        const juce::ScopedLock sl (streamLock);
        if (threadShouldExit())
            return r;                               // shutting down — never even start the request
        activeStream = &web;                        // publish for cross-thread cancel()
    }

    const bool connected = web.connect (nullptr);   // no network / DNS / TLS failure → false
    juce::String body;
    if (connected && web.getStatusCode() == 200)    // 404 = no releases yet, 403 = rate-limited, etc.
        body = web.readEntireStreamAsString();

    {
        const juce::ScopedLock sl (streamLock);
        activeStream = nullptr;                     // retire BEFORE the stack-scope stream dies
    }

    if (threadShouldExit() || body.isEmpty())
        return r;

    const juce::var json = juce::JSON::parse (body);
    if (! json.isObject())
        return r;

    // /releases/latest already excludes drafts + pre-releases, so tag_name is a real vX.Y.Z. Strip
    // the leading 'v' (stored/compared bare); the plugin decides "outdated" itself against kDescribe.
    const juce::String tag = stripV (json.getProperty ("tag_name", juce::var()).toString());
    if (tag.isEmpty())
        return r;

    r.ok       = true;
    r.latest   = tag;
    r.url      = json.getProperty ("html_url", juce::var()).toString();   // the release page
    r.notes    = json.getProperty ("name",     juce::var()).toString();   // release title (optional)
    r.outdated = update::remoteIsNewer (current.toStdString(), tag.toStdString());
    return r;
}

void UpdateChecker::run()
{
    const Result r = fetch();
    if (threadShouldExit())
        return;                                     // shutting down — drop the result silently
    pending = r;                                    // visible to handleAsyncUpdate: the message post synchronizes
    triggerAsyncUpdate();
}

void UpdateChecker::handleAsyncUpdate()
{
    // Message thread. Persist first (PropertiesFile is not thread-safe), then notify the popover.
    if (pending.ok && pending.outdated && pending.latest.isNotEmpty())
        storeOutdated (pending.latest);

    if (auto cb = std::exchange (onDone, nullptr))
        cb (pending);
}

void UpdateChecker::checkNow (std::function<void (Result)> cb)
{
    JUCE_ASSERT_MESSAGE_THREAD
    onDone = std::move (cb);
    if (! isThreadRunning())
        startThread();                              // a finished worker restarts cleanly
}

} // namespace tabby
