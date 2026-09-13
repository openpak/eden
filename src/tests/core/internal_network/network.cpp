// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/internal_network/network.h"
#include "core/internal_network/sockets.h"

TEST_CASE("Network::Errors", "[core]") {
    Network::NetworkInstance network_instance; // initialize network

    Network::Socket socks[2];
    for (Network::Socket& sock : socks) {
        REQUIRE(sock.Initialize(Network::Domain::INET, Network::Type::STREAM,
                                Network::Protocol::TCP) == Network::Errno::SUCCESS);
    }

    Network::SockAddrIn addr{
        Network::Domain::INET,
        {127, 0, 0, 1},
        1, // hopefully nobody running this test has something listening on port 1
    };
    REQUIRE(socks[0].Connect(addr) == Network::Errno::CONNREFUSED);

    std::vector<u8> message{1, 2, 3, 4};
    REQUIRE(socks[1].Recv(0, message).second == Network::Errno::NOTCONN);
}

TEST_CASE("Network::TcpNoDelay", "[core]") {
    Network::NetworkInstance network_instance;
    Network::Socket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::STREAM,
                              Network::Protocol::TCP) == Network::Errno::SUCCESS);
    for (const bool enabled : {true, false}) {
        REQUIRE(socket.SetNoDelay(enabled) == Network::Errno::SUCCESS);
        const auto [value, error] = socket.GetNoDelay();
        REQUIRE(error == Network::Errno::SUCCESS);
        REQUIRE(value == static_cast<u32>(enabled));
    }
}

TEST_CASE("Network::SocketOptionReadback", "[core]") {
    Network::NetworkInstance network_instance;
    Network::Socket socket;
    REQUIRE(socket.Initialize(Network::Domain::INET, Network::Type::STREAM,
                              Network::Protocol::TCP) == Network::Errno::SUCCESS);
    for (const bool enabled : {true, false}) {
        REQUIRE(socket.SetReuseAddr(enabled) == Network::Errno::SUCCESS);
        {
            const auto [value, error] = socket.GetReuseAddr();
            REQUIRE(error == Network::Errno::SUCCESS);
            REQUIRE(value == static_cast<u32>(enabled));
        }
        REQUIRE(socket.SetKeepAlive(enabled) == Network::Errno::SUCCESS);
        {
            const auto [value, error] = socket.GetKeepAlive();
            REQUIRE(error == Network::Errno::SUCCESS);
            REQUIRE(value == static_cast<u32>(enabled));
        }
        REQUIRE(socket.SetBroadcast(enabled) == Network::Errno::SUCCESS);
        {
            const auto [value, error] = socket.GetBroadcast();
            REQUIRE(error == Network::Errno::SUCCESS);
            REQUIRE(value == static_cast<u32>(enabled));
        }
    }
    // Kernels may round or double buffer sizes; verify the effective capacity.
    REQUIRE(socket.SetSndBuf(8192) == Network::Errno::SUCCESS);
    {
        const auto [value, error] = socket.GetSndBuf();
        REQUIRE(error == Network::Errno::SUCCESS);
        REQUIRE(value >= 8192);
    }
    REQUIRE(socket.SetRcvBuf(8192) == Network::Errno::SUCCESS);
    {
        const auto [value, error] = socket.GetRcvBuf();
        REQUIRE(error == Network::Errno::SUCCESS);
        REQUIRE(value >= 8192);
    }
}
