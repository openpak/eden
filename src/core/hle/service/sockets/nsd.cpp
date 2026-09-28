// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/nsd.h"

#include "common/string_util.h"

namespace Service::Sockets {

constexpr Result ResultPermissionDenied{ErrorModule::NSD, 3};
constexpr Result ResultOverflow{ErrorModule::NSD, 6};

// This is nn::oe::ServerEnvironmentType
enum class ServerEnvironmentType : u8 {
    Dd,
    Lp,
    Sd,
    Sp,
    Dp,
};

// This is nn::nsd::EnvironmentIdentifier
struct EnvironmentIdentifier {
    std::array<u8, 8> identifier;
};
static_assert(sizeof(EnvironmentIdentifier) == 0x8);

NSD::NSD(Core::System& system_, const char* name) : ServiceFramework{system_, name} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {5, &NSD::GetEmptyName, "GetSettingUrl"},
        {10, &NSD::GetEmptyName, "GetSettingName"},
        {11, &NSD::GetEnvironmentIdentifier, "GetEnvironmentIdentifier"},
        {12, &NSD::GetDeviceId, "GetDeviceId"},
        {13, &NSD::StubSuccess, "DeleteSettings"},
        {14, &NSD::StubSuccess, "ImportSettings"},
        {15, &NSD::SetChangeEnvironmentIdentifierDisabled, "SetChangeEnvironmentIdentifierDisabled"},
        {20, &NSD::Resolve, "Resolve"},
        {21, &NSD::ResolveEx, "ResolveEx"},
        {30, &NSD::GetNasServiceSetting, "GetNasServiceSetting"},
        {31, &NSD::GetNasServiceSettingEx, "GetNasServiceSettingEx"},
        {40, &NSD::GetEmptyName, "GetNasRequestFqdn"},
        {41, &NSD::GetEmptyNameEx, "GetNasRequestFqdnEx"},
        {42, &NSD::GetEmptyName, "GetNasApiFqdn"},
        {43, &NSD::GetEmptyNameEx, "GetNasApiFqdnEx"},
        {50, &NSD::GetSaveData, "GetCurrentSetting"},
        {51, &NSD::StubSuccessPrivileged, "WriteTestParameter"},
        {52, &NSD::ReadTestParameter, "ReadTestParameter"},
        {60, &NSD::GetSaveData, "ReadSaveDataFromFsForTest"},
        {61, &NSD::StubSuccessPrivileged, "WriteSaveDataToFsForTest"},
        {62, &NSD::StubSuccessPrivileged, "DeleteSaveDataOfFsForTest"},
        {63, &NSD::IsChangeEnvironmentIdentifierDisabled, "IsChangeEnvironmentIdentifierDisabled"},
        {64, &NSD::StubSuccess, "SetWithoutDomainExchangeFqdns"},
        {100, &NSD::GetApplicationServerEnvironmentType, "GetApplicationServerEnvironmentType"},
        {101, &NSD::StubSuccess, "SetApplicationServerEnvironmentType"},
        {102, &NSD::StubSuccess, "DeleteApplicationServerEnvironmentType"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

std::string NsdResolve(const std::string& fqdn_in) {
    // A console asks for names with a '%' where the environment goes, and nsd is what fills it
    // in before anything resolves them -- so "nncs2-%.n.n.srv.nintendo.net" is a different host
    // from "nncs1-%..." only after this runs. Names below are passed through as hardware does.
    if (fqdn_in == "api.sect.srv.nintendo.net" || fqdn_in == "ctest.cdn.nintendo.net" ||
        fqdn_in == "ctest.cdn.n.nintendoswitch.cn" || fqdn_in == "unknown.dummy.nintendo.net") {
        return fqdn_in;
    }

    std::string fqdn = fqdn_in;
    for (std::size_t pos = fqdn.find('%'); pos != std::string::npos; pos = fqdn.find('%', pos + 3)) {
        fqdn.replace(pos, 1, "lp1");
    }

    // [OpenPak] The account names of both environments (Ryujinx FqdnResolver.cs).
    if (fqdn == "e97b8a9d672e4ce4845ec6947cd66ef6-sb-api.accounts.nintendo.com" ||
        fqdn == "e97b8a9d672e4ce4845ec6947cd66ef6-sb.accounts.nintendo.com") {
        return "e97b8a9d672e4ce4845ec6947cd66ef6-sb.baas.nintendo.com";
    }
    if (fqdn == "api.accounts.nintendo.com" || fqdn == "accounts.nintendo.com") {
        return "e0d67c509fb203858ebcb2fe3f88c2aa.baas.nintendo.com";
    }

    return fqdn;
}

static std::string ResolveImpl(const std::string& fqdn_in) {
    const std::string resolved = NsdResolve(fqdn_in);
    LOG_DEBUG(Service, "called, fqdn_in={} -> {}", fqdn_in, resolved);
    return resolved;
}

static Result ResolveCommon(const std::string& fqdn_in, std::array<char, 0x100>& fqdn_out) {
    const auto res = ResolveImpl(fqdn_in);
    if (res.size() >= fqdn_out.size()) {
        return ResultOverflow;
    }
    std::memcpy(fqdn_out.data(), res.c_str(), res.size() + 1);
    return ResultSuccess;
}

void NSD::SetChangeEnvironmentIdentifierDisabled(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const bool disabled = rp.Pop<bool>();

    LOG_WARNING(Service, "(STUBBED) called, disabled={}", disabled);

    IPC::ResponseBuilder rb{ctx, 1};
    rb.Push(ResultSuccess);
}

void NSD::Resolve(HLERequestContext& ctx) {
    const std::string fqdn_in = Common::StringFromBuffer(ctx.ReadBuffer(0));

    std::array<char, 0x100> fqdn_out{};
    const Result res = ResolveCommon(fqdn_in, fqdn_out);

    ctx.WriteBuffer(fqdn_out);
    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(res);
}

void NSD::ResolveEx(HLERequestContext& ctx) {
    const std::string fqdn_in = Common::StringFromBuffer(ctx.ReadBuffer(0));

    std::array<char, 0x100> fqdn_out;
    const Result res = ResolveCommon(fqdn_in, fqdn_out);

    if (res.IsError()) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(res);
        return;
    }

    ctx.WriteBuffer(fqdn_out);
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(ResultSuccess);
}

void NSD::GetEnvironmentIdentifier(HLERequestContext& ctx) {
    constexpr EnvironmentIdentifier lp1 = {
        .identifier = {'l', 'p', '1', '\0', '\0', '\0', '\0', '\0'}};
    ctx.WriteBuffer(lp1);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void NSD::GetApplicationServerEnvironmentType(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push(static_cast<u32>(ServerEnvironmentType::Lp));
}

// [OpenPak] The commands below are answered with empty settings rather than left unimplemented
// (as Citron has them). Those of the test and save-data group are nsd:a's alone.
void NSD::GetEmptyName(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    ctx.WriteBuffer(std::array<u8, 0x100>{}); // nn::nsd::Url, SettingName, Fqdn

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void NSD::GetEmptyNameEx(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    ctx.WriteBuffer(std::array<u8, 0x100>{}); // nn::nsd::Fqdn

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(ResultSuccess); // nn::nsd::InnerResult
}

void NSD::GetDeviceId(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    ctx.WriteBuffer(std::array<u8, 0x10>{}); // nn::nsd::DeviceId

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void NSD::GetNasServiceSetting(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    ctx.WriteBuffer(std::array<u8, 0x108>{}); // nn::nsd::NasServiceSetting

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void NSD::GetNasServiceSettingEx(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    ctx.WriteBuffer(std::array<u8, 0x108>{}); // nn::nsd::NasServiceSetting

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push(ResultSuccess); // nn::nsd::InnerResult
}

void NSD::GetSaveData(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    if (GetServiceName() != "nsd:a") {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultPermissionDenied);
        return;
    }

    ctx.WriteBuffer(std::vector<u8>(0x12BF0)); // nn::nsd::SaveData

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void NSD::ReadTestParameter(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    if (GetServiceName() != "nsd:a") {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultPermissionDenied);
        return;
    }

    ctx.WriteBuffer(std::array<u8, 0x80>{}); // nn::nsd::detail::TestParameter

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void NSD::StubSuccessPrivileged(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(GetServiceName() == "nsd:a" ? ResultSuccess : ResultPermissionDenied);
}

void NSD::IsChangeEnvironmentIdentifierDisabled(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<u8>(false);
}

void NSD::StubSuccess(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

NSD::~NSD() = default;

} // namespace Service::Sockets
