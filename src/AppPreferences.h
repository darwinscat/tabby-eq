// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_data_structures/juce_data_structures.h>   // ApplicationProperties / PropertiesFile / InterProcessLock

namespace tabby
{

//==============================================================================
// tabby::AppPreferences — TabbyEQ's single GLOBAL PropertiesFile: per-machine, shared across every
// plugin instance, surviving restarts (NOT the DAW session state that getStateInformation writes).
// Today it backs only the UpdateChecker's last-seen release tag (so the "update available" badge
// persists across sessions and is shared across instances); future app-wide view-prefs land here too.
//
// Sharing model (deliberate deviation from orbitcab, which holds a per-instance AppPreferences —
// two instances there each cache their own PropertiesFile, so a badge stored by one is stale in the
// other until reconstruction, and concurrent saves can clobber; noted upstream):
//   • SAME PROCESS: the processor holds this via juce::SharedResourcePointer, so every instance in
//     the host process shares ONE PropertiesFile object — no same-process staleness or clobbering.
//   • CROSS PROCESS: options.processLock (an InterProcessLock) serializes file writes between host
//     processes, so saves can't corrupt each other. Reads are refreshed via reload() at
//     UpdateChecker construction; between checks a value written by ANOTHER process is stale until
//     the next reload/check — accepted + documented (the badge is advisory, not safety-critical).
//
// Test isolation: the TABBYEQ_PREFS_DIR env var (absolute path) redirects the store to that
// directory — the headless ctest harnesses set it so constructing the real processor never touches
// (or pollutes) the developer's real settings file.
//
// Adapter layer (host glue: storage). The teq:: DSP core never sees it.
//==============================================================================
class AppPreferences
{
public:
    AppPreferences()
    {
        juce::PropertiesFile::Options o;
        o.applicationName     = "TabbyEQ";
        o.folderName          = "Darwin's Cat" + juce::String (juce::File::getSeparatorString()) + "TabbyEQ";
        o.filenameSuffix      = "settings";
        o.osxLibrarySubFolder = "Application Support";
        o.processLock         = &ipLock;   // cross-process write serialization (see the header note)

        // Test isolation: TABBYEQ_PREFS_DIR (absolute) redirects the store away from the real
        // per-user settings file. Used by the ctest harnesses; ignored when unset/relative.
        const juce::String overrideDir = juce::SystemStats::getEnvironmentVariable ("TABBYEQ_PREFS_DIR", {});
        if (overrideDir.isNotEmpty() && juce::File::isAbsolutePath (overrideDir))
        {
            juce::File dir (overrideDir);
            dir.createDirectory();
            overrideFile = std::make_unique<juce::PropertiesFile> (dir.getChildFile ("TabbyEQ.settings"), o);
        }
        else
        {
            props.setStorageParameters (o);
        }
    }

    // Small string pref (the UpdateChecker's last-seen release tag).
    juce::String getString (juce::StringRef key, const juce::String& defaultValue = {}) const
    {
        if (auto* s = file()) return s->getValue (key, defaultValue);
        return defaultValue;
    }

    void setString (juce::StringRef key, const juce::String& value)
    {
        if (auto* s = file()) { s->setValue (key, value); s->saveIfNeeded(); }
    }

    // Re-read the file from disk to pick up ANOTHER PROCESS's writes (same-process instances share
    // this object, and every setter here saves immediately, so no same-process state can be lost).
    void reload()
    {
        if (auto* s = file()) s->reload();
    }

    // Raw PropertiesFile access for richer needs (the UpdateChecker's tag + epoch + removeValue).
    // Logically const — getUserSettings() is non-const in JUCE.
    juce::PropertiesFile* file() const
    {
        if (overrideFile != nullptr) return overrideFile.get();
        return const_cast<AppPreferences*> (this)->props.getUserSettings();
    }

private:
    // Declared FIRST: the PropertiesFile keeps a pointer to this lock, so it must outlive both
    // stores below (members are destroyed in reverse declaration order).
    juce::InterProcessLock ipLock { "DarwinsCat.TabbyEQ.settings" };

    juce::ApplicationProperties props;                    // the normal per-user store
    std::unique_ptr<juce::PropertiesFile> overrideFile;   // the TABBYEQ_PREFS_DIR test store (or null)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppPreferences)
};

} // namespace tabby
