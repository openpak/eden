// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "common/settings.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/nsd.h"
#include "core/hle/service/sockets/sfdnsres.h"
#include "openpak/network_profile.h"
#include "core/hle/service/sockets/sockets.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/memory.h"

namespace Service::Sockets {

SFDNSRES::SFDNSRES(Core::System& system_) : ServiceFramework{system_, "sfdnsres"} {
    static const FunctionInfo functions[] = {
        {0, nullptr, "SetDnsAddressesPrivateRequest"},
        {1, nullptr, "GetDnsAddressPrivateRequest"},
        {2, &SFDNSRES::GetHostByNameRequest, "GetHostByNameRequest"},
        {3, nullptr, "GetHostByAddrRequest"},
        {4, nullptr, "GetHostStringErrorRequest"},
        {5, &SFDNSRES::GetGaiStringErrorRequest, "GetGaiStringErrorRequest"},
        {6, &SFDNSRES::GetAddrInfoRequest, "GetAddrInfoRequest"},
        {7, nullptr, "GetNameInfoRequest"},
        {8, nullptr, "RequestCancelHandleRequest"},
        {9, nullptr, "CancelRequest"},
        {10, &SFDNSRES::GetHostByNameRequestWithOptions, "GetHostByNameRequestWithOptions"},
        {11, nullptr, "GetHostByAddrRequestWithOptions"},
        {12, &SFDNSRES::GetAddrInfoRequestWithOptions, "GetAddrInfoRequestWithOptions"},
        {13, nullptr, "GetNameInfoRequestWithOptions"},
        {14, &SFDNSRES::ResolverSetOptionRequest, "ResolverSetOptionRequest"},
        {15, nullptr, "ResolverGetOptionRequest"},
    };
    RegisterHandlers(functions);
}

SFDNSRES::~SFDNSRES() = default;

enum class NetDbError : s32 {
    Internal = -1,
    Success = 0,
    HostNotFound = 1,
    TryAgain = 2,
    NoRecovery = 3,
    NoData = 4,
};

static const constexpr std::array blockedDomains = {
    "srv.nintendo.net", //obvious
    "nintendo.es",
    "nintendowifi.net",
    "nintendo-europe.com",
    "nintendo.com.hk",
    "nintendo.com.au",
    "nintendo.co.kr",
    "nintendo.co.uk",
    "nintendo.co.jp",
    "nintendo.co.nz",
    "nintendo.co.za",
    "nintendo.com",
    "nintendo.jp",
    "nintendo.tw",
    "nintendo.at",
    "nintendo.be",
    "nintendo.dk",
    "nintendo.de",
    "nintendo.fi",
    "nintendo.fr",
    "nintendo.gr",
    "nintendo.hu",
    "nintendo.it",
    "nintendo.nl",
    "nintendo.no",
    "nintendo.pt",
    "nintendo.ru",
    "nintendo.ch",
    "nintendo.se",
    "nintendoswitch.com.cn",
    "nintendoswitch.com",
    "sun.hac.lp1.d4c.nintendo.net",
    "phoenix-api.wbagora.com", //hogwarts legacy
    "battle.net",
    "microsoft.com", // Minecraft dungeons + other games
    "mojang.com",
    "xboxlive.com",
    "api.epicgames.dev", // marvel cosmic invasion +?
    "minecraftservices.com",
    "508223012e5a5ff19f30a391b2bdadc0.my.2k.com", // Civilization 5
};

static bool IsBlockedHost(const std::string& host) {
    return std::any_of(
        blockedDomains.begin(), blockedDomains.end(),
        [&host](const std::string& domain) { return host.find(domain) != std::string::npos; });
}

// [OpenPak] A title's own online hostnames are answered with the OpenPak server's address, so
// the guest talks to us without any patch to the game or the firmware. Off unless enabled; the
// env vars exist because frontends that never surface the setting (SDL, CI, a test rig) still
// need a way in.
static bool OpenPakActive() {
    if (Settings::values.enable_openpak.GetValue()) {
        return true;
    }
    const char* env = std::getenv("OPENPAK_ENABLE");
    if (env == nullptr || *env == '\0') {
        return false;
    }
    const std::string value = Common::ToLower(env);
    return value != "0" && value != "false" && value != "no" && value != "off";
}

static std::string ConfiguredIp(const std::string& setting, const char* env_var) {
    if (!setting.empty()) {
        return setting;
    }
    if (const char* env = std::getenv(env_var); env != nullptr && *env != '\0') {
        return env;
    }
    return {};
}

bool IsNintendoHost(std::string_view host) {
    static constexpr std::string_view domains[] = {"nintendo.net", "nintendo.com",
                                                   "nintendowifi.net", "nintendo.co.jp"};
    return std::any_of(std::begin(domains), std::end(domains), [host](std::string_view domain) {
        return host == domain ||
               (host.size() > domain.size() && host.ends_with(domain) &&
                host[host.size() - domain.size() - 1] == '.');
    });
}

bool IsNatCheckHost(std::string_view host) {
    return host.starts_with("nncs2-") && host.ends_with(".n.n.srv.nintendo.net");
}

static std::optional<std::string> GetOpenPakRedirectIp(std::string_view host) {
    if (!OpenPakActive()) {
        return std::nullopt;
    }
    const std::string server_ip =
        ConfiguredIp(Settings::values.openpak_server_ip.GetValue(), "OPENPAK_SERVER_IP");
    if (server_ip.empty()) {
        return std::nullopt;
    }
    // The profile OpenPak publishes decides which names are redirected and where, because it is
    // generated from the live routing: a title served on a new hostname works without a new
    // build. Two things it says that a wildcard cannot: a name with an address of its own (the
    // NAT check compares what two addresses observe of one console, so its second probe must not
    // collapse onto the first), and a name that must be left alone entirely -- the console's own
    // connection test measures OpenPak instead of the internet if it is redirected.
    if (const auto from_profile =
            openpak::client::profile::RedirectFor(std::string{host}, server_ip);
        from_profile.has_value()) {
        return from_profile;
    }

    if (openpak::client::profile::Loaded()) {
        // A profile in hand and no match means the name is not ours to answer.
        return std::nullopt;
    }

    if (IsNatCheckHost(host)) {
        const std::string nat_ip =
            ConfiguredIp(Settings::values.openpak_nat_ip.GetValue(), "OPENPAK_NAT_IP");
        return nat_ip.empty() ? server_ip : nat_ip;
    }
    if (IsNintendoHost(host)) {
        return server_ip;
    }
    return std::nullopt;
}

static NetDbError GetAddrInfoErrorToNetDbError(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        return NetDbError::Success;
    case GetAddrInfoError::AGAIN:
        return NetDbError::TryAgain;
    case GetAddrInfoError::NODATA:
        return NetDbError::HostNotFound;
    case GetAddrInfoError::SERVICE:
        return NetDbError::Success;
    default:
        return NetDbError::HostNotFound;
    }
}

static Errno GetAddrInfoErrorToErrno(GetAddrInfoError result) {
    // These combinations have been verified on console (but are not
    // exhaustive).
    switch (result) {
    case GetAddrInfoError::SUCCESS:
        // Note: Sometimes a successful lookup sets errno to EADDRNOTAVAIL for
        // some reason, but that doesn't seem useful to implement.
        return Errno::SUCCESS;
    case GetAddrInfoError::AGAIN:
        return Errno::SUCCESS;
    case GetAddrInfoError::NODATA:
        return Errno::SUCCESS;
    case GetAddrInfoError::SERVICE:
        return Errno::INVAL;
    default:
        return Errno::SUCCESS;
    }
}

template <typename T>
static void Append(std::vector<u8>& vec, T t) {
    const size_t offset = vec.size();
    vec.resize(offset + sizeof(T));
    std::memcpy(vec.data() + offset, &t, sizeof(T));
}

static void AppendNulTerminated(std::vector<u8>& vec, std::string_view str) {
    const size_t offset = vec.size();
    vec.resize(offset + str.size() + 1);
    std::memmove(vec.data() + offset, str.data(), str.size());
}

// We implement gethostbyname using the host's getaddrinfo rather than the
// host's gethostbyname, because it simplifies portability: e.g., getaddrinfo
// behaves the same on Unix and Windows, unlike gethostbyname where Windows
// doesn't implement h_errno.
static std::vector<u8> SerializeAddrInfoAsHostEnt(const std::vector<Network::AddrInfo>& vec,
                                                  std::string_view host) {

    std::vector<u8> data;
    // h_name: use the input hostname (append nul-terminated)
    AppendNulTerminated(data, host);
    // h_aliases: leave empty

    Append<u32_be>(data, 0); // count of h_aliases
    // (If the count were nonzero, the aliases would be appended as nul-terminated here.)
    Append<u16_be>(data, static_cast<u16>(Domain::INET)); // h_addrtype
    Append<u16_be>(data, sizeof(Network::IPv4Address));   // h_length
    // h_addr_list:
    size_t count = vec.size();
    ASSERT(count <= UINT32_MAX);
    Append<u32_be>(data, static_cast<uint32_t>(count));
    for (const Network::AddrInfo& addrinfo : vec) {
        // On the Switch, this is passed through htonl despite already being
        // big-endian, so it ends up as little-endian.
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip));

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }
    return data;
}

static std::pair<u32, GetAddrInfoError> GetHostByNameRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_DEBUG(Service, "called: use_nsd_resolve={}, cancel_handle={}, process_id={}",
              parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    std::string host = Common::StringFromBuffer(host_buffer);
    // For now, ignore options, which are in input buffer 1 for GetHostByNameRequestWithOptions.

    if (parameters.use_nsd_resolve != 0) {
        std::string resolved = NsdResolve(host);
        if (resolved != host) {
            LOG_DEBUG(Network, "nsd resolved '{}' -> '{}'", host, resolved);
            host = std::move(resolved);
        }
    }

    // [OpenPak] Redirection wins over the blocklist: these are exactly the hosts the blocklist
    // exists to stop, and pointing them at our own server is the point.
    std::string query_host = host;
    if (const auto redirect = GetOpenPakRedirectIp(host); redirect.has_value()) {
        LOG_INFO(Network, "[OpenPak] Redirecting '{}' -> '{}'", host, *redirect);
        query_host = *redirect;
    } else if (IsBlockedHost(host)) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    auto res_v = Network::GetAddressInfo(query_host, /*service*/ std::nullopt);
    if (auto* res = std::get_if<std::vector<Network::AddrInfo>>(&res_v)) {
        const std::vector<u8> data = SerializeAddrInfoAsHostEnt(*res, host);
        const u32 data_size = u32(data.size());
        ctx.WriteBuffer(data, 0);
        return {data_size, GetAddrInfoError::SUCCESS};
    }
    auto* err = std::get_if<Network::GetAddrInfoError>(&res_v);
    return {0, Translate(*err)};
}

void SFDNSRES::GetHostByNameRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        NetDbError netdb_error;
        Errno bsd_errno;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .data_size = data_size,
    });
}

void SFDNSRES::GetHostByNameRequestWithOptions(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetHostByNameRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

static std::vector<u8> SerializeAddrInfo(const std::vector<Network::AddrInfo>& vec,
                                         std::string_view host) {
    // Adapted from
    // https://github.com/switchbrew/libnx/blob/c5a9a909a91657a9818a3b7e18c9b91ff0cbb6e3/nx/source/runtime/resolver.c#L190
    std::vector<u8> data;

    for (const Network::AddrInfo& addrinfo : vec) {
        // serialized addrinfo:
        Append<u32_be>(data, 0xBEEFCAFE);                                        // magic
        Append<u32_be>(data, 0);                                                 // ai_flags
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.family)));      // ai_family
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.socket_type))); // ai_socktype
        Append<u32_be>(data, static_cast<u32>(Translate(addrinfo.protocol)));    // ai_protocol
        Append<u32_be>(data, sizeof(SockAddrIn)); // ai_addrlen

        // ai_addr: a BSD sockaddr_in, the SockAddrIn struct in sockets.h --
        // {u8 sin_len; u8 sin_family; u16 sin_port; u8 sin_addr[4]; u8 sin_zero[8];}.
        //
        // [OpenPak] sin_len is its own byte and must carry the full sockaddr size. Writing
        // sin_family as one 2-byte big-endian value folds sin_len away as an implicit zero, and
        // a gRPC title -- NPLN is gRPC -- builds its own connect() sockaddr straight out of this
        // buffer: off a sin_len of 0 it reads the address and port out of the wrong offsets and
        // dials a port nobody is listening on, so the connection never completes and the title
        // sits on its transport deadline. Citron and Ryujinx both had to fix the same byte.
        Append<u8>(data, static_cast<u8>(sizeof(SockAddrIn)));              // sin_len
        Append<u8>(data, static_cast<u8>(Translate(addrinfo.addr.family))); // sin_family
        // On the Switch, the following fields are passed through htonl despite
        // already being big-endian, so they end up as little-endian.
        Append<u16_le>(data, addrinfo.addr.portno);                            // sin_port
        Append<u32_le>(data, Network::IPv4AddressToInteger(addrinfo.addr.ip)); // sin_addr
        data.resize(data.size() + 8, 0);                                       // sin_zero

        if (addrinfo.canon_name.has_value()) {
            AppendNulTerminated(data, *addrinfo.canon_name);
        } else {
            data.push_back(0);
        }

        LOG_INFO(Service, "Resolved host '{}' to IPv4 address {}", host,
                 Network::IPv4AddressToString(addrinfo.addr.ip));
    }

    data.resize(data.size() + 4, 0); // 4-byte sentinel value

    return data;
}


// [OpenPak] The answer shaped the way a console's own resolver shapes it: one entry per address,
// socket type and protocol left "any".
//
// The hints a title passes are ignored above, so the host's getaddrinfo is free to answer with
// one entry per socket type -- stream, datagram and raw for a single address. A title that walks
// that list looking only for what it asked for finds nothing it can use and gives up before it
// opens a socket: Stardew stops at 2318-0007 with the address resolved and no connection ever
// attempted. Ryujinx has always written "0 = Any" here, which is why the same title connects
// there. Off with the integration, so nothing else changes shape.
static std::vector<Network::AddrInfo> OpenPakAddrInfo(const std::vector<Network::AddrInfo>& vec) {
    if (!OpenPakActive()) {
        return vec;
    }

    std::vector<Network::AddrInfo> out;

    for (const Network::AddrInfo& entry : vec) {
        const bool already = std::any_of(out.begin(), out.end(), [&](const Network::AddrInfo& seen) {
            return seen.addr.ip == entry.addr.ip && seen.addr.portno == entry.addr.portno;
        });

        if (already) {
            continue;
        }

        Network::AddrInfo copy = entry;
        copy.socket_type = Network::Type::Unspecified;
        copy.protocol = Network::Protocol::Unspecified;

        out.push_back(copy);
    }

    return out;
}

static std::pair<u32, GetAddrInfoError> GetAddrInfoRequestImpl(HLERequestContext& ctx) {
    struct InputParameters {
        u8 use_nsd_resolve;
        u32 cancel_handle;
        u64 process_id;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    IPC::RequestParser rp{ctx};
    const auto parameters = rp.PopRaw<InputParameters>();

    LOG_DEBUG(Service, "called: use_nsd_resolve={}, cancel_handle={}, process_id={}",
              parameters.use_nsd_resolve, parameters.cancel_handle, parameters.process_id);

    const auto host_buffer = ctx.ReadBuffer(0);
    std::string host = Common::StringFromBuffer(host_buffer);

    // Hardware routes these through nsd first, which is where the '%' in a name becomes the
    // environment. Skipping it leaves distinct services sharing one name -- the NAT check's two
    // probes being the case that matters, since they must land on two different addresses.
    if (parameters.use_nsd_resolve != 0) {
        std::string resolved = NsdResolve(host);
        if (resolved != host) {
            LOG_DEBUG(Network, "nsd resolved '{}' -> '{}'", host, resolved);
            host = std::move(resolved);
        }
    }

    // [OpenPak] Hold the FIRST npln resolution of the session until the startup translation
    // storm has passed. A title's NPLN channel that comes up mid-storm parks without ever
    // sending its first RPC -- the title looks online and freezes. Measured as the Nextendo
    // npln retention on the Ryujinx side of this integration, where waiting out the burst was
    // the difference between a working channel and a startup block. One hold per process, and
    // only for the guest's npln names.
    static std::once_flag npln_hold;
    if (host.find("npln") != std::string::npos) {
        std::call_once(npln_hold, [] {
            LOG_INFO(Network, "[OpenPak] Holding the first npln resolution for the startup "
                              "burst to pass");
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        });
    }

    // [OpenPak] Redirection wins over the blocklist: these are exactly the hosts the blocklist
    // exists to stop, and pointing them at our own server is the point.
    std::string query_host = host;
    bool redirected = false;
    if (const auto redirect = GetOpenPakRedirectIp(host); redirect.has_value()) {
        LOG_INFO(Network, "[OpenPak] Redirecting '{}' -> '{}'", host, *redirect);
        query_host = *redirect;
        redirected = true;
    } else if (IsBlockedHost(host)) {
        LOG_WARNING(Network, "Resolution of hostname {} requested, returning EAI_AGAIN", host);
        return {0, GetAddrInfoError::AGAIN};
    }

    std::optional<std::string> service = std::nullopt;
    if (ctx.CanReadBuffer(1)) {
        const std::span<const u8> service_buffer = ctx.ReadBuffer(1);
        service = Common::StringFromBuffer(service_buffer);
    }

    // Serialized hints are also passed in a buffer, but are ignored for now.

    auto res_v = Network::GetAddressInfo(query_host, service);
    if (auto* res = std::get_if<std::vector<Network::AddrInfo>>(&res_v)) {
        // [OpenPak] A redirect resolves a literal address, and a literal has no canonical name to
        // report -- so the answer carried an empty one where the title expected the name it
        // asked for. An HTTP/2 title builds its :authority from that name: NPLN completed TCP,
        // TLS and the h2 handshake and then never sent a HEADERS frame, because the authority it
        // had to send was empty. The name the title asked for is the canonical name here.
        if (redirected) {
            for (Network::AddrInfo& entry : *res) {
                entry.canon_name = host;
            }
        }
        const std::vector<u8> data = SerializeAddrInfo(OpenPakAddrInfo(*res), host);
        const u32 data_size = u32(data.size());
        ctx.WriteBuffer(data, 0);
        return {data_size, GetAddrInfoError::SUCCESS};
    }
    auto* err = std::get_if<Network::GetAddrInfoError>(&res_v);
    return {0, Translate(*err)};
}

void SFDNSRES::GetAddrInfoRequest(HLERequestContext& ctx) {
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        Errno bsd_errno;
        GetAddrInfoError gai_error;
        u32 data_size;
    };
    static_assert(sizeof(OutputParameters) == 0xc);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
        .gai_error = emu_gai_err,
        .data_size = data_size,
    });
}

void SFDNSRES::GetGaiStringErrorRequest(HLERequestContext& ctx) {
    struct InputParameters {
        GetAddrInfoError gai_errno;
    };
    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    const std::string result = Translate(input.gai_errno);
    ctx.WriteBuffer(result);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void SFDNSRES::GetAddrInfoRequestWithOptions(HLERequestContext& ctx) {
    // Additional options are ignored
    auto [data_size, emu_gai_err] = GetAddrInfoRequestImpl(ctx);

    struct OutputParameters {
        u32 data_size;
        GetAddrInfoError gai_error;
        NetDbError netdb_error;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x10);

    IPC::ResponseBuilder rb{ctx, 6};
    rb.Push(ResultSuccess);
    rb.PushRaw(OutputParameters{
        .data_size = data_size,
        .gai_error = emu_gai_err,
        .netdb_error = GetAddrInfoErrorToNetDbError(emu_gai_err),
        .bsd_errno = GetAddrInfoErrorToErrno(emu_gai_err),
    });
}

void SFDNSRES::ResolverSetOptionRequest(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};

    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}


DNS_PRIV::DNS_PRIV(Core::System& system_)
    : ServiceFramework{system_, "dns:priv"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "Cmd0"},
        {1, nullptr, "Cmd1"},
        {2, nullptr, "Cmd2"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

DNS_PRIV::~DNS_PRIV() = default;


} // namespace Service::Sockets
