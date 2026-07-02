// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <juce_data_structures/juce_data_structures.h>   // ApplicationProperties / PropertiesFile

namespace tabby
{

//==============================================================================
// tabby::AppPreferences — owns TabbyEQ's single GLOBAL PropertiesFile: per-machine, shared across
// every plugin instance, surviving restarts (NOT the DAW session state that getStateInformation
// writes). One owner is the whole point — two PropertiesFile instances on the same path would race
// on save. Today it backs only the UpdateChecker's last-seen release tag (so the "update available"
// badge persists across sessions and is shared across instances); future app-wide view-prefs land
// here too. Mirrors orbitcab's AppPreferences (same storage choice for the update badge).
//
// Adapter layer (host glue: storage). The teq:: DSP core never sees it.
//==============================================================================
class AppPreferences
{
public:
    AppPreferences()
    {
        // Global (NOT the DAW session) — shared across instances, survives restarts.
        juce::PropertiesFile::Options o;
        o.applicationName     = "TabbyEQ";
        o.folderName          = "Darwin's Cat" + juce::String (juce::File::getSeparatorString()) + "TabbyEQ";
        o.filenameSuffix      = "settings";
        o.osxLibrarySubFolder = "Application Support";
        props.setStorageParameters (o);
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

    // Raw PropertiesFile access for richer needs (the UpdateChecker's tag + epoch + removeValue).
    // Logically const — getUserSettings() is non-const in JUCE.
    juce::PropertiesFile* file() const
    {
        return const_cast<AppPreferences*> (this)->props.getUserSettings();
    }

private:
    juce::ApplicationProperties props;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppPreferences)
};

} // namespace tabby
