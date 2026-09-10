// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/sockets/sfdnsres.h"

using Service::Sockets::IsNatCheckHost;
using Service::Sockets::IsNintendoHost;

TEST_CASE("OpenPak redirect claims Nintendo hosts only", "[openpak]") {
    REQUIRE(IsNintendoHost("nintendo.net"));
    REQUIRE(IsNintendoHost("api.accounts.nintendo.com"));
    REQUIRE(IsNintendoHost("dauth-lp1.ndas.srv.nintendo.net"));
    REQUIRE(IsNintendoHost("conntest.nintendowifi.net"));

    // Suffix match must stop at a label boundary, or a lookalike domain gets hijacked.
    REQUIRE_FALSE(IsNintendoHost("notnintendo.net"));
    REQUIRE_FALSE(IsNintendoHost("nintendo.net.example.com"));
    REQUIRE_FALSE(IsNintendoHost("ns.photonengine.io"));
    REQUIRE_FALSE(IsNintendoHost(""));

    REQUIRE(IsNatCheckHost("nncs2-lp1.n.n.srv.nintendo.net"));
    REQUIRE_FALSE(IsNatCheckHost("dauth-lp1.ndas.srv.nintendo.net"));
}
