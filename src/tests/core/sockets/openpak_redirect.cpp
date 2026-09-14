// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/socket_types.h"
#include "core/hle/service/sockets/nsd.h"
#include "core/hle/service/sockets/sfdnsres.h"

using Service::Sockets::IsNatCheckHost;
using Service::Sockets::IsNintendoHost;

TEST_CASE("Resolver address length matches its IPv4 wire record", "[openpak][sockets]") {
    const Network::AddrInfo entry{
        .family = Network::Domain::INET,
        .socket_type = Network::Type::STREAM,
        .protocol = Network::Protocol::TCP,
        .addr = {.family = Network::Domain::INET, .ip = {192, 0, 2, 1}, .portno = 443},
        .canon_name = "farm.example",
    };
    const auto bytes = Service::Sockets::SerializeAddrInfo({entry, entry}, "farm.example");
    // Each header is 24 bytes, followed by a 16-byte IPv4 sockaddr and the
    // null-terminated canonical name. A four-byte sentinel ends the list.
    constexpr size_t record_size = 24 + 16 + 13;
    REQUIRE(bytes.size() == 2 * record_size + 4);
    for (size_t start : {size_t{0}, record_size}) {
        const u32 address_length = (u32{bytes[start + 20]} << 24) |
                                   (u32{bytes[start + 21]} << 16) |
                                   (u32{bytes[start + 22]} << 8) | bytes[start + 23];
        REQUIRE(address_length == 16);
        REQUIRE(bytes[start + 24] == 16);
        REQUIRE(bytes[start + 25] == 2);
        REQUIRE(std::string_view(reinterpret_cast<const char*>(bytes.data() + start + 24 +
                                                               address_length), 12) ==
                "farm.example");
    }
    REQUIRE(bytes[record_size] == 0xbe);
    REQUIRE(bytes[2 * record_size] == 0);
}

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

TEST_CASE("nsd fills the environment in before anything resolves", "[openpak]") {
    using Service::Sockets::NsdResolve;

    // The case the NAT check depends on: two probes that must not collapse onto one name.
    REQUIRE(NsdResolve("nncs1-%.n.n.srv.nintendo.net") == "nncs1-lp1.n.n.srv.nintendo.net");
    REQUIRE(NsdResolve("nncs2-%.n.n.srv.nintendo.net") == "nncs2-lp1.n.n.srv.nintendo.net");

    REQUIRE(NsdResolve("accounts.nintendo.com") == "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com");
    REQUIRE(NsdResolve("ctest.cdn.nintendo.net") == "ctest.cdn.nintendo.net");
    REQUIRE(NsdResolve("dauth-%.ndas.srv.nintendo.net") == "dauth-lp1.ndas.srv.nintendo.net");
    REQUIRE(NsdResolve("no-substitution.example") == "no-substitution.example");
}
