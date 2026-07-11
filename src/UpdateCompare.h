// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2026 Darwin's Cat — Oleh Tsymaienko <oleh@darwinscat.com> & Alisa <alisa@darwinscat.com>. Part of TabbyEQ — see LICENSE.

#pragma once

// The update-check comparison rule was PROMOTED verbatim to the family's shared felitronics-appkit
// (it is the same rule OrbitCab now uses). This alias shim keeps every call site and
// tests/update_compare.cpp compiling unchanged — the family's standard shim pattern.
#include <felitronics/appkit/UpdateCompare.h>

namespace tabby
{
    namespace update = felitronics::appkit::update;
}
