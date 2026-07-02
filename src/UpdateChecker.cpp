// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#include "UpdateChecker.h"
#include "UpdateCompare.h"   // the pure, JUCE-free compare rule (also unit-tested off the plugin)

namespace tabby
{

namespace
{
    constexpr int  kTimeoutMs     = 5000;   // short — opt-in, non-blocking
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
    : current (std::move (currentDescribe)), prefs (prefsRef)
{
    // Badge-clear: if the running build has caught up to (or passed) a previously seen "latest",
    // drop the stored value so no stale badge shows. The ONLY place the plugin compares at ctor time.
    const juce::String seen = storedLatest();
    if (seen.isNotEmpty() && ! update::remoteIsNewer (current.toStdString(), seen.toStdString()))
        clearStored();
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

UpdateChecker::Result UpdateChecker::fetch (const juce::String& describe)
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
    if (! web.connect (nullptr))            // no network / DNS / TLS failure
        return r;
    if (web.getStatusCode() != 200)         // 404 = no releases yet, 403 = rate-limited, etc.
        return r;

    const juce::var json = juce::JSON::parse (web.readEntireStreamAsString());
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
    r.outdated = update::remoteIsNewer (describe.toStdString(), tag.toStdString());
    return r;
}

void UpdateChecker::checkNow (std::function<void (Result)> onDone)
{
    const juce::String ver = current;                       // capture by value (thread-safe)
    juce::WeakReference<UpdateChecker> weak (this);
    juce::Thread::launch ([weak, ver, onDone]
    {
        const Result r = fetch (ver);                       // static — touches no instance state
        // Store + notify on the message thread (PropertiesFile + UI are not RT/thread-safe).
        juce::MessageManager::callAsync ([weak, onDone, r]
        {
            if (auto* self = weak.get())                    // no-op if the plugin was removed meanwhile
                if (r.ok && r.outdated && r.latest.isNotEmpty())
                    self->storeOutdated (r.latest);
            if (onDone)
                onDone (r);
        });
    });
}

} // namespace tabby
