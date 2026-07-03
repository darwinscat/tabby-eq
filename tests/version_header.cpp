// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.
//
// A tiny JUCE-free sanity check on the build-time-baked TabbyVersion.h: the build number is a valid
// 14-digit UTC stamp and the string constants are present. This guards the CMake generator (a stale
// or malformed stamp fails here rather than shipping). Runs in the plugin-OFF core-tests ctest.

#include "TabbyVersion.h"

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

static void check (bool ok, const char* what)
{
    if (! ok) { std::printf ("FAIL: %s\n", what); ++failures; }
    else        std::printf ("ok:   %s\n", what);
}

int main()
{
    using namespace tabby::version;

    // kBuildNumber: exactly 14 digits, YYYYMMDDHHMMSS, and a plausible calendar value.
    const long long bn = kBuildNumber;
    check (bn >= 10000000000000LL && bn <= 99999999999999LL, "kBuildNumber is a 14-digit long long");

    const int sec   = (int) (bn % 100);
    const int minute= (int) (bn / 100 % 100);
    const int hour  = (int) (bn / 10000 % 100);
    const int day   = (int) (bn / 1000000 % 100);
    const int month = (int) (bn / 100000000 % 100);
    const int year  = (int) (bn / 10000000000LL);
    check (year  >= 2020 && year <= 9999, "kBuildNumber year in range");
    check (month >= 1 && month <= 12,     "kBuildNumber month in range");
    check (day   >= 1 && day   <= 31,     "kBuildNumber day in range");
    check (hour  >= 0 && hour  <= 23,     "kBuildNumber hour in range");
    check (minute>= 0 && minute<= 59,     "kBuildNumber minute in range");
    check (sec   >= 0 && sec   <= 60,     "kBuildNumber second in range");   // 60 tolerates a leap second

    // String constants are non-null and non-empty.
    check (kDescribe    != nullptr && std::strlen (kDescribe)    > 0, "kDescribe non-empty");
    check (kGitHash     != nullptr && std::strlen (kGitHash)     > 0, "kGitHash non-empty");
    check (kBuilder     != nullptr && std::strlen (kBuilder)     > 0, "kBuilder non-empty");
    check (kCoreVersion != nullptr && std::strlen (kCoreVersion) > 0, "kCoreVersion non-empty");

    // kGitDirty is a bool — just exercise it so the constant is odr-used.
    check (kGitDirty == true || kGitDirty == false, "kGitDirty is a bool");

    std::printf ("\nTabbyVersion: describe=\"%s\" build=%lld hash=\"%s\" dirty=%d builder=\"%s\" core=\"%s\"\n",
                 kDescribe, kBuildNumber, kGitHash, (int) kGitDirty, kBuilder, kCoreVersion);
    std::printf (failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
