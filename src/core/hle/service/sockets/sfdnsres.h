// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>
#include <vector>

#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Network {
struct AddrInfo;
}

namespace Service::Sockets {

// [OpenPak] Which hostnames the OpenPak redirect claims. Exposed for the unit test.
bool IsNintendoHost(std::string_view host);
bool IsNatCheckHost(std::string_view host);

std::vector<u8> SerializeAddrInfo(const std::vector<Network::AddrInfo>& entries,
                                 std::string_view host);

class SFDNSRES final : public ServiceFramework<SFDNSRES> {
public:
    explicit SFDNSRES(Core::System& system_);
    ~SFDNSRES() override;

private:
    void GetHostByNameRequest(HLERequestContext& ctx);
    void GetGaiStringErrorRequest(HLERequestContext& ctx);
    void GetHostByNameRequestWithOptions(HLERequestContext& ctx);
    void GetAddrInfoRequest(HLERequestContext& ctx);
    void GetAddrInfoRequestWithOptions(HLERequestContext& ctx);
    void ResolverSetOptionRequest(HLERequestContext& ctx);
};

class DNS_PRIV final : public ServiceFramework<DNS_PRIV> {
public:
    explicit DNS_PRIV(Core::System& system_);
    ~DNS_PRIV() override;
};

} // namespace Service::Sockets
