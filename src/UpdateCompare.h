// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

#include <string_view>

//==============================================================================
// The update-check comparison rule — a PURE, header-only, JUCE-FREE function so it can be unit-
// tested off the plugin (tests/update_compare.cpp rides the plugin-OFF core ctest, next to the
// version-header test). It answers one question the UpdateChecker needs: given the running build's
// `git describe` stamp (TabbyVersion.h kDescribe) and the latest GitHub release tag, should the
// "update available" badge light?
//
// The running version is NOT a clean semver like orbitcab's JucePlugin_VersionString — it is a
// `git describe --tags --always --dirty` string. That is the one real adaptation from orbitcab's
// checker, and it drives the rule below:
//
//   • A CLEAN RELEASE build stamps kDescribe as exactly "vX.Y.Z" (CI checks out the tag; the tree
//     is clean, so describe emits the bare tag). These compare numerically against the remote tag.
//   • A DEV build stamps something else — "0.0.0-dev"/"unknown" (untagged tree), "vX.Y.Z-N-gHASH"
//     (N commits past the tag), or a "-dirty" suffix. By decision, a dev build counts OLDER than
//     ANY release, so the badge lights whenever a real release exists. (Consequence: a developer
//     running a between-releases build always sees the badge once a release is published — intended:
//     "you are not on a released build.")
//
// End users only ever run clean-tag CI builds, so they get correct numeric semver behaviour.
namespace tabby::update
{
    struct Semver { int major = 0, minor = 0, patch = 0; };

    // Hostile-input bound: a version component may carry at most 6 digits (max 999999 — far beyond
    // any real release). More digits marks the whole string UNPARSEABLE ({0,0,0} / not clean), so a
    // hostile tag can never sign-overflow the int accumulation (999999 * 10 + 9 << INT_MAX).
    inline constexpr int kMaxComponentDigits = 6;

    // Parse major.minor.patch from the LEADING part of a version string. A single leading 'v'/'V'
    // is stripped; scanning stops at the first char that is neither a digit nor a '.', so
    // "v0.1.0-78-g7ba0138" -> {0,1,0}, "0.0.0-dev" -> {0,0,0}, "unknown" -> {0,0,0}. Missing
    // components default to 0. Any component longer than kMaxComponentDigits rejects the whole
    // string to {0,0,0} (see the bound above).
    constexpr Semver parseSemver (std::string_view s) noexcept
    {
        if (! s.empty() && (s.front() == 'v' || s.front() == 'V'))
            s.remove_prefix (1);

        Semver out {};
        int idx = 0, val = 0, digits = 0;                       // idx: 0=major 1=minor 2=patch
        auto commit = [&] { if (idx == 0) out.major = val; else if (idx == 1) out.minor = val; else if (idx == 2) out.patch = val; };
        for (char c : s)
        {
            if (c >= '0' && c <= '9')
            {
                if (++digits > kMaxComponentDigits) return {};  // hostile/absurd component → unparseable
                val = val * 10 + (c - '0');
            }
            else if (c == '.') { commit(); if (++idx > 2) return out; val = 0; digits = 0; }
            else               break;                           // '-', '+', letter, space … : end of the numeric version
        }
        commit();
        return out;
    }

    // Is `describe` an exact, clean release tag "vX.Y.Z" (nothing after the patch)? Any commits-ahead
    // suffix ("-N-g…"), "-dirty", pre-release text, "0.0.0-dev" or "unknown" makes it NOT clean → a dev
    // build. An over-long component (see kMaxComponentDigits) is also not clean, keeping this
    // consistent with parseSemver's rejection.
    constexpr bool isCleanRelease (std::string_view s) noexcept
    {
        if (! s.empty() && (s.front() == 'v' || s.front() == 'V'))
            s.remove_prefix (1);
        if (s.empty()) return false;

        int dots = 0, digits = 0;
        bool digitInPart = false;
        for (char c : s)
        {
            if      (c >= '0' && c <= '9') { digitInPart = true; if (++digits > kMaxComponentDigits) return false; }
            else if (c == '.') { if (! digitInPart) return false; ++dots; digitInPart = false; digits = 0; }
            else               return false;                    // any '-', letter, etc. → not a clean release
        }
        return dots == 2 && digitInPart;                        // exactly major.minor.patch, patch present
    }

    // Should the badge light? True iff `remoteTag` is a real release that is NEWER than the build
    // identified by `localDescribe` (kDescribe). See the file header for the dev-build rule.
    constexpr bool remoteIsNewer (std::string_view localDescribe, std::string_view remoteTag) noexcept
    {
        const Semver r = parseSemver (remoteTag);
        if (r.major == 0 && r.minor == 0 && r.patch == 0)
            return false;                                       // no usable release tag → nothing to offer

        if (! isCleanRelease (localDescribe))
            return true;                                        // dev build: older than ANY release

        const Semver l = parseSemver (localDescribe);
        if (r.major != l.major) return r.major > l.major;
        if (r.minor != l.minor) return r.minor > l.minor;
        return r.patch > l.patch;
    }
}
