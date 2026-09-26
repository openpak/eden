// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <poll.h>
#endif

#include <fmt/ranges.h>

#include "common/logging.h"
#include "common/socket_types.h"
#include "core/core.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/sockets/bsd.h"
#include "core/hle/service/sockets/interface_list.h"
#include "core/internal_network/network_interface.h"
#include "core/hle/service/sockets/sockets_translate.h"
#include "core/internal_network/network.h"
#include "core/internal_network/socket_proxy.h"
#include "core/internal_network/sockets.h"
#include "network/network.h"
#include <common/settings.h>

namespace Service::Sockets {

namespace {

bool IsConnectionBased(Type type) {
    switch (type) {
    case Type::STREAM:
        return true;
    case Type::DGRAM:
        return false;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return false;
    }
}

template <typename T>
T GetValue(std::span<const u8> buffer) {
    T t{};
    std::memcpy(&t, buffer.data(), (std::min)(sizeof(T), buffer.size()));
    return t;
}

template <typename T>
void PutValue(std::span<u8> buffer, const T& t) {
    std::memcpy(buffer.data(), &t, (std::min)(sizeof(T), buffer.size()));
}

} // Anonymous namespace

void BSD_USA::PollWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->PollImpl(write_buffer, read_buffer, nfds, timeout);
}

void BSD_USA::PollWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::AcceptWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->AcceptImpl(fd, write_buffer);
}

void BSD_USA::AcceptWork::Response(HLERequestContext& ctx) {
    if (write_buffer.size() > 0) {
        ctx.WriteBuffer(write_buffer);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::ConnectWork::Execute(BSD_USA* bsd) {
    bsd_errno = bsd->ConnectImpl(fd, addr);
}

void BSD_USA::ConnectWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RecvWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvImpl(fd, flags, message);
}

void BSD_USA::RecvWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RecvFromWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->RecvFromImpl(fd, flags, message, addr);
}

void BSD_USA::RecvFromWork::Response(HLERequestContext& ctx) {
    ctx.WriteBuffer(message, 0);
    if (!addr.empty()) {
        ctx.WriteBuffer(addr, 1);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(addr.size()));
}

void BSD_USA::SendWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendImpl(fd, flags, message);
}

void BSD_USA::SendWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SendToWork::Execute(BSD_USA* bsd) {
    std::tie(ret, bsd_errno) = bsd->SendToImpl(fd, flags, message, addr);
}

void BSD_USA::SendToWork::Response(HLERequestContext& ctx) {
    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RegisterClient(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 3};

    rb.Push(ResultSuccess);
    rb.Push<s32>(0); // bsd errno
}

void BSD_USA::StartMonitoring(HLERequestContext& ctx) {
    LOG_WARNING(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 2};

    rb.Push(ResultSuccess);
}

void BSD_USA::Socket(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    const auto [fd, bsd_errno] = SocketImpl(Domain(domain), Type(type), Protocol(protocol));

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SocketExempt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const u32 domain = rp.Pop<u32>();
    const u32 type = rp.Pop<u32>();
    const u32 protocol = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. domain={} type={} protocol={}", domain, type, protocol);

    auto [fd, bsd_errno] = SocketImpl(Domain(domain), Type(type), Protocol(protocol));
    if (bsd_errno == Errno::SUCCESS) {
        bsd_errno = ShutdownImpl(fd, 0);
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::Select(HLERequestContext& ctx) {
    LOG_DEBUG(Service, "(STUBBED) called");

    IPC::ResponseBuilder rb{ctx, 4};

    rb.Push(ResultSuccess);
    rb.Push<u32>(0); // ret
    rb.Push<u32>(0); // bsd errno
}

void BSD_USA::Poll(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 nfds = rp.Pop<s32>();
    const s32 timeout = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. nfds={} timeout={}", nfds, timeout);

    // [OpenPak] The deferred poll. gRPC parks a thread in Poll() with its wakeup eventfd in the
    // set and expects a *different* guest thread's eventfd Write() to end the wait -- a wait
    // answered synchronously here would hold this worker thread, and the Write() IPC would
    // queue behind the very wait it is meant to end. Instead: one non-blocking pass now, and if
    // nothing is ready yet, give the thread up (SetIsDeferred) and let the deferral event's
    // heartbeat re-run this handler until something genuinely is. Measured as the one repair
    // that carries a working Stardew online session on the Nextendo builds.
    std::vector<u8> read_buffer;
    bool had_snapshot = false;
    std::optional<std::chrono::steady_clock::time_point> existing_deadline;
    {
        std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
        if (const auto it = deferred_poll_snapshots.find(&ctx); it != deferred_poll_snapshots.end()) {
            read_buffer = it->second.read_buffer;
            existing_deadline = it->second.deadline;
            had_snapshot = true;
        }
    }
    if (!had_snapshot) {
        const auto live_buffer = ctx.ReadBuffer();
        read_buffer.assign(live_buffer.begin(), live_buffer.end());
    }

    if (timeout != 0 && GetBsdDeferralEvent() != nullptr &&
        PollSetIncludesEventFd(read_buffer, nfds)) {
        std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
        auto [ret, bsd_errno] = PollImpl(write_buffer, read_buffer, nfds, /*timeout=*/0);

        // An expired poll succeeds with zero ready descriptors.
        if (ret == 0 && bsd_errno == Errno::SUCCESS && existing_deadline &&
            std::chrono::steady_clock::now() >= *existing_deadline) {
            std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
            deferred_poll_snapshots.erase(&ctx);
            if (write_buffer.size() > 0) {
                ctx.WriteBuffer(write_buffer);
            }
            IPC::ResponseBuilder rb{ctx, 4};
            rb.Push(ResultSuccess);
            rb.Push<s32>(0);
            rb.PushEnum(Errno::SUCCESS);
            return;
        }

        if (ret == 0 && bsd_errno == Errno::SUCCESS) {
            // Nothing ready yet -- give up this thread rather than block it. ServerManager
            // re-invokes this handler when the deferral event fires, reading the snapshot
            // rather than guest memory another IPC may since have reused.
            if (!had_snapshot) {
                std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
                const auto deadline =
                    timeout == -1
                        ? std::optional<std::chrono::steady_clock::time_point>{}
                        : std::optional{std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(timeout)};
                deferred_poll_snapshots[&ctx] = DeferredPollState{read_buffer, deadline};
            }
            LOG_DEBUG(Service, "[OpenPak] Poll deferred (nfds={} timeout={}), eventfd in set",
                      nfds, timeout);
            ctx.SetIsDeferred();
            return;
        }
        // Something to report (or an error): drop any snapshot and answer now, with the same
        // shape an ordinary Poll reply has.
        if (had_snapshot) {
            std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
            deferred_poll_snapshots.erase(&ctx);
        }
        if (write_buffer.size() > 0) {
            ctx.WriteBuffer(write_buffer);
        }
        IPC::ResponseBuilder rb{ctx, 4};
        rb.Push(ResultSuccess);
        rb.Push<s32>(ret);
        rb.PushEnum(bsd_errno);
        return;
    }

    // Not taking the deferred path -- no stale snapshot may linger under this ctx.
    if (had_snapshot) {
        std::scoped_lock snapshot_lock{deferred_poll_snapshot_mutex};
        deferred_poll_snapshots.erase(&ctx);
    }

    ExecuteWork(ctx, PollWork{
                         .nfds = nfds,
                         .timeout = timeout,
                         .read_buffer = read_buffer,
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::Accept(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    ExecuteWork(ctx, AcceptWork{
                         .fd = fd,
                         .write_buffer = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::Bind(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());
    BuildErrnoResponse(ctx, BindImpl(fd, ctx.ReadBuffer()));
}

void BSD_USA::Connect(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} addrlen={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, ConnectWork{
                         .fd = fd,
                         .addr = ctx.ReadBuffer(),
                     });
}

void BSD_USA::GetPeerName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetPeerNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::GetSockName(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    std::vector<u8> write_buffer(ctx.GetWriteBufferSize());
    const Errno bsd_errno = GetSockNameImpl(fd, write_buffer);

    ctx.WriteBuffer(write_buffer);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno != Errno::SUCCESS ? -1 : 0);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(static_cast<u32>(write_buffer.size()));
}

void BSD_USA::GetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const auto optname = static_cast<OptName>(rp.Pop<u32>());

    std::vector<u8> optval(ctx.GetWriteBufferSize());

    LOG_DEBUG(Service, "called. fd={} level={} optname={:#x} len={:#x}", fd, level, optname,
              optval.size());

    const Errno err = GetSockOptImpl(fd, level, optname, optval);

    ctx.WriteBuffer(optval);

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(err == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(err);
    rb.Push<u32>(static_cast<u32>(optval.size()));
}

void BSD_USA::Sysctl(HLERequestContext& ctx) {
    // [OpenPak] getifaddrs(): NPLN's WebRTC gathers connection candidates from it. Ported from
    // Ryujinx; every other query stays unsupported, as before, but now answers instead of
    // falling through to an unimplemented command.
    const auto mib_bytes = ctx.ReadBuffer(0);
    std::vector<s32> mib(std::min<std::size_t>(mib_bytes.size() / sizeof(s32), 16));
    std::memcpy(mib.data(), mib_bytes.data(), mib.size() * sizeof(s32));
    const std::size_t new_size = ctx.CanReadBuffer(1) ? ctx.GetReadBufferSize(1) : 0;
    const std::size_t old_size = ctx.CanWriteBuffer(0) ? ctx.GetWriteBufferSize(0) : 0;

    s32 ret = -1;
    Errno bsd_errno = Errno::OPNOTSUPP;
    u32 length = 0;

    const auto iface = Network::GetSelectedNetworkInterface();
    if (InterfaceList::Matches(mib) && new_size == 0 && iface) {
        const auto list = InterfaceList::Build(Network::TranslateIPv4(iface->ip_address),
                                               Network::TranslateIPv4(iface->subnet_mask));
        length = static_cast<u32>(list.size());
        if (old_size != 0 && old_size < list.size()) {
            bsd_errno = Errno::NOMEM; // no buffer is the size probe; a short one is ENOMEM
        } else {
            if (old_size != 0) {
                ctx.WriteBuffer(list);
            }
            ret = 0;
            bsd_errno = Errno::SUCCESS;
        }
    } else {
        LOG_WARNING(Service, "(STUBBED) Sysctl mib=[{}] old={} new={}", fmt::join(mib, ","),
                    old_size, new_size);
    }

    IPC::ResponseBuilder rb{ctx, 5};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
    rb.Push<u32>(length);
}

void BSD_USA::Listen(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 backlog = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} backlog={}", fd, backlog);

    BuildErrnoResponse(ctx, ListenImpl(fd, backlog));
}

void BSD_USA::Fcntl(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 cmd = rp.Pop<s32>();
    const s32 arg = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} cmd={} arg={}", fd, cmd, arg);

    const auto [ret, bsd_errno] = FcntlImpl(fd, static_cast<FcntlCmd>(cmd), arg);

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::SetSockOpt(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 level = rp.Pop<u32>();
    const OptName optname = static_cast<OptName>(rp.Pop<u32>());
    const auto optval = ctx.ReadBuffer();

    LOG_DEBUG(Service, "called. fd={} level={} optname={:#x} optlen={}", fd, level,
              static_cast<u32>(optname), optval.size());

    const Errno bsd_errno = SetSockOptImpl(fd, level, optname, optval);
    if (bsd_errno != Errno::SUCCESS) {
        // Named, because a bare "not supported" says nothing about whether it matters: the one
        // that cost a detour elsewhere was SO_NOSIGPIPE, which is harmless.
        LOG_WARNING(Service, "setsockopt fd={} level={:#x} optname={:#x} ({} bytes) failed: {}", fd,
                    level, static_cast<u32>(optname), optval.size(), bsd_errno);
    }
    BuildErrnoResponse(ctx, bsd_errno);
}


// [OpenPak] sendmmsg/recvmmsg, the scatter-gather send a gRPC title uses for every frame it
// writes. NPLN is gRPC, so leaving these unimplemented meant the title panicked (2010-0212) the
// moment its TCP connection came up and it tried to write the first TLS record: the connection
// was fine, there was simply no way to put bytes on it.
//
// The buffer is a receive descriptor the guest hands over already filled in: a leading byte
// Nintendo itself ignores, then one packed msghdr per message --
//   u32 name_len, name bytes, u32 iov_count, {u64 iov_len, iov bytes}*, u32 control_len,
//   control bytes, u32 flags, u32 length
// -- and the same layout is written back with each message's length set to what was transferred.
namespace {

struct MMsgHdr {
    std::vector<u8> name;
    std::vector<std::vector<u8>> iov;
    std::vector<u8> control;
    u32 flags{};
    u32 length{};
};

bool Take(std::span<const u8>& data, void* out, size_t size) {
    if (data.size() < size) {
        return false;
    }
    std::memcpy(out, data.data(), size);
    data = data.subspan(size);
    return true;
}

bool TakeBytes(std::span<const u8>& data, std::vector<u8>& out, size_t size) {
    if (data.size() < size) {
        return false;
    }
    out.assign(data.begin(), data.begin() + size);
    data = data.subspan(size);
    return true;
}

bool DeserializeMMsg(std::vector<MMsgHdr>& out, std::span<const u8> data, s32 vlen) {
    if (vlen < 0 || data.empty()) {
        return false;
    }
    data = data.subspan(1); // the header byte hardware ignores

    out.resize(static_cast<size_t>(vlen));
    for (MMsgHdr& msg : out) {
        u32 name_len{};
        if (!Take(data, &name_len, sizeof(name_len)) || !TakeBytes(data, msg.name, name_len)) {
            return false;
        }
        u32 iov_count{};
        if (!Take(data, &iov_count, sizeof(iov_count))) {
            return false;
        }
        msg.iov.resize(iov_count);
        for (std::vector<u8>& iov : msg.iov) {
            u64 iov_len{};
            if (!Take(data, &iov_len, sizeof(iov_len)) ||
                !TakeBytes(data, iov, static_cast<size_t>(iov_len))) {
                return false;
            }
        }
        u32 control_len{};
        if (!Take(data, &control_len, sizeof(control_len)) ||
            !TakeBytes(data, msg.control, control_len)) {
            return false;
        }
        if (!Take(data, &msg.flags, sizeof(msg.flags)) ||
            !Take(data, &msg.length, sizeof(msg.length))) {
            return false;
        }
    }
    return true;
}

void Put(std::vector<u8>& out, const void* value, size_t size) {
    const auto* bytes = static_cast<const u8*>(value);
    out.insert(out.end(), bytes, bytes + size);
}

std::vector<u8> SerializeMMsg(const std::vector<MMsgHdr>& msgs) {
    std::vector<u8> out;
    out.push_back(8); // what hardware writes back here
    for (const MMsgHdr& msg : msgs) {
        const u32 name_len = static_cast<u32>(msg.name.size());
        Put(out, &name_len, sizeof(name_len));
        out.insert(out.end(), msg.name.begin(), msg.name.end());

        const u32 iov_count = static_cast<u32>(msg.iov.size());
        Put(out, &iov_count, sizeof(iov_count));
        for (const std::vector<u8>& iov : msg.iov) {
            const u64 iov_len = iov.size();
            Put(out, &iov_len, sizeof(iov_len));
            out.insert(out.end(), iov.begin(), iov.end());
        }

        const u32 control_len = static_cast<u32>(msg.control.size());
        Put(out, &control_len, sizeof(control_len));
        out.insert(out.end(), msg.control.begin(), msg.control.end());

        Put(out, &msg.flags, sizeof(msg.flags));
        Put(out, &msg.length, sizeof(msg.length));
    }
    return out;
}

/// Spread the byte count a single send/recv moved back over the messages it covered, and answer
/// with how many messages it reached. A message that is only partly filled still counts: a
/// stream read almost never fills the room offered, and answering "none" for 2214 bytes into an
/// 8 KiB buffer tells the caller nothing arrived.
s32 SpreadTransferred(std::vector<MMsgHdr>& msgs, size_t transferred) {
    if (transferred == 0) {
        return 0;
    }

    size_t index = 0;
    size_t left = transferred;

    while (left > 0 && index < msgs.size()) {
        MMsgHdr& msg = msgs[index];

        size_t capacity = 0;
        for (const std::vector<u8>& iov : msg.iov) {
            capacity += iov.size();
        }

        size_t stored;
        if (left > capacity) {
            stored = capacity;
            ++index;
        } else {
            stored = left;
        }

        msg.length = static_cast<u32>(stored);
        left -= stored;
    }

    return static_cast<s32>((std::min)(index + 1, msgs.size()));
}

/// The opening bytes of a payload, so a handshake that is refused can be read back rather than
/// guessed at: a TLS record says its type and version in the first five bytes.
std::string HexHead(std::span<const u8> data, size_t count = 48) {
    std::string out;
    for (size_t i = 0; i < (std::min)(count, data.size()); ++i) {
        out += fmt::format("{:02x}", data[i]);
    }
    return out;
}

/// A named or control-carrying message is a datagram shape this cannot express as one stream
/// send; hardware titles only use the plain form, so say so rather than send the wrong bytes.
bool IsPlain(const std::vector<MMsgHdr>& msgs) {
    return std::all_of(msgs.begin(), msgs.end(), [](const MMsgHdr& msg) {
        return msg.name.empty() && msg.control.empty();
    });
}

} // Anonymous namespace

void BSD_USA::SendMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 vlen = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    std::vector<MMsgHdr> msgs;
    if (!DeserializeMMsg(msgs, ctx.ReadBufferB(), vlen)) {
        LOG_ERROR(Service, "SendMMsg fd={} vlen={}: malformed message buffer", fd, vlen);
        BuildErrnoResponse(ctx, Errno::INVAL);
        return;
    }
    if (!IsPlain(msgs)) {
        LOG_WARNING(Service, "SendMMsg fd={} vlen={}: named or control message", fd, vlen);
        BuildErrnoResponse(ctx, Errno::NOPROTOOPT);
        return;
    }

    std::vector<u8> payload;
    for (const MMsgHdr& msg : msgs) {
        for (const std::vector<u8>& iov : msg.iov) {
            payload.insert(payload.end(), iov.begin(), iov.end());
        }
    }

    s32 sent = 0;
    Errno bsd_errno = Errno::SUCCESS;
    if (!payload.empty()) {
        std::tie(sent, bsd_errno) = SendImpl(fd, flags, payload);
    }

    LOG_DEBUG(Service, "SendMMsg fd={} vlen={} bytes={} -> {} errno {} head={}", fd, vlen,
              payload.size(), sent, static_cast<u32>(bsd_errno), HexHead(payload));

    s32 ret = -1;
    if (bsd_errno == Errno::SUCCESS) {
        ret = SpreadTransferred(msgs, static_cast<size_t>((std::max)(sent, 0)));
        const std::vector<u8> written = SerializeMMsg(msgs);
        ctx.WriteBufferB(written.data(), written.size());
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::RecvMMsg(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const s32 vlen = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    std::vector<MMsgHdr> msgs;
    if (!DeserializeMMsg(msgs, ctx.ReadBufferB(), vlen)) {
        LOG_ERROR(Service, "RecvMMsg fd={} vlen={}: malformed message buffer", fd, vlen);
        BuildErrnoResponse(ctx, Errno::INVAL);
        return;
    }
    if (!IsPlain(msgs)) {
        LOG_WARNING(Service, "RecvMMsg fd={} vlen={}: named or control message", fd, vlen);
        BuildErrnoResponse(ctx, Errno::NOPROTOOPT);
        return;
    }

    size_t capacity = 0;
    for (const MMsgHdr& msg : msgs) {
        for (const std::vector<u8>& iov : msg.iov) {
            capacity += iov.size();
        }
    }

    std::vector<u8> buffer(capacity);
    s32 received = 0;
    Errno bsd_errno = Errno::SUCCESS;
    if (capacity > 0) {
        std::tie(received, bsd_errno) = RecvImpl(fd, flags, buffer);
    }

    LOG_DEBUG(Service, "RecvMMsg fd={} vlen={} room={} -> {} errno {} head={}", fd, vlen, capacity,
              received, static_cast<u32>(bsd_errno),
              HexHead({buffer.data(), static_cast<size_t>((std::max)(received, 0))}));

    s32 ret = -1;
    if (bsd_errno == Errno::SUCCESS) {
        size_t offset = 0;
        for (MMsgHdr& msg : msgs) {
            for (std::vector<u8>& iov : msg.iov) {
                const size_t take =
                    (std::min)(iov.size(), static_cast<size_t>((std::max)(received, 0)) - offset);
                std::memcpy(iov.data(), buffer.data() + offset, take);
                offset += take;
                if (offset >= static_cast<size_t>((std::max)(received, 0))) {
                    break;
                }
            }
            if (offset >= static_cast<size_t>((std::max)(received, 0))) {
                break;
            }
        }
        ret = SpreadTransferred(msgs, static_cast<size_t>((std::max)(received, 0)));
        const std::vector<u8> written = SerializeMMsg(msgs);
        ctx.WriteBufferB(written.data(), written.size());
    }

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(ret);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::Shutdown(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const s32 how = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} how={}", fd, how);

    BuildErrnoResponse(ctx, ShutdownImpl(fd, how));
}

void BSD_USA::Recv(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={}", fd, flags, ctx.GetWriteBufferSize());

    ExecuteWork(ctx, RecvWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::RecvFrom(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={} addrlen={}", fd, flags,
              ctx.GetWriteBufferSize(0), ctx.GetWriteBufferSize(1));

    ExecuteWork(ctx, RecvFromWork{
                         .fd = fd,
                         .flags = flags,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize(0)),
                         .addr = std::vector<u8>(ctx.GetWriteBufferSize(1)),
                     });
}

void BSD_USA::Send(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};

    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags={:#x} len={}", fd, flags, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(),
                     });
}

void BSD_USA::SendTo(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();
    const u32 flags = rp.Pop<u32>();

    LOG_DEBUG(Service, "called. fd={} flags=0x{} len={} addrlen={}", fd, flags,
              ctx.GetReadBufferSize(0), ctx.GetReadBufferSize(1));

    ExecuteWork(ctx, SendToWork{
                         .fd = fd,
                         .flags = flags,
                         .message = ctx.ReadBuffer(0),
                         .addr = ctx.ReadBuffer(1),
                     });
}

void BSD_USA::Write(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetReadBufferSize());

    ExecuteWork(ctx, SendWork{
                         .fd = fd,
                         .flags = 0,
                         .message = ctx.ReadBuffer(),
                     });
}

// [OpenPak] read(2) on a descriptor, which the stub answered with "zero bytes, no error" --
// indistinguishable from end of file. Write was already a real send; only this side was missing,
// so a title could poll its event fd but never drain it, and Pia waits on exactly that: it parks
// until the fd signals and reads to clear it. A read that always returns nothing is a wait that
// never ends.
void BSD_USA::Read(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={} len={}", fd, ctx.GetWriteBufferSize());

    ExecuteWork(ctx, RecvWork{
                         .fd = fd,
                         .flags = 0,
                         .message = std::vector<u8>(ctx.GetWriteBufferSize()),
                     });
}

void BSD_USA::Close(HLERequestContext& ctx) {
    IPC::RequestParser rp{ctx};
    const s32 fd = rp.Pop<s32>();

    LOG_DEBUG(Service, "called. fd={}", fd);

    BuildErrnoResponse(ctx, CloseImpl(fd));
}

/// @brief Only bsd:s is able to dup()
void BSD_USA::DuplicateSocket(HLERequestContext& ctx) {
    struct InputParameters {
        s32 fd;
        u64 reserved;
    };
    static_assert(sizeof(InputParameters) == 0x10);

    struct OutputParameters {
        s32 ret;
        Errno bsd_errno;
    };
    static_assert(sizeof(OutputParameters) == 0x8);

    IPC::RequestParser rp{ctx};
    auto input = rp.PopRaw<InputParameters>();

    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    if (is_user) {
        rb.PushRaw(OutputParameters{
            .ret = 0,
            .bsd_errno = Errno::INVAL,
        });
        return;
    }

    auto const res_v = DuplicateSocketImpl(input.fd);
    if (auto* res = std::get_if<s32>(&res_v)) {
        rb.PushRaw(OutputParameters{
            .ret = *res,
            .bsd_errno = Errno::SUCCESS,
        });
    } else {
        auto* err = std::get_if<Errno>(&res_v);
        rb.PushRaw(OutputParameters{
            .ret = 0,
            .bsd_errno = *err,
        });
    }
}

// [OpenPak] An event fd a title can actually wait on.
//
// The stub answered "success" and no descriptor, so the guest took the returned 0 as its fd --
// the same number its real socket already had -- and then polled a handle nobody could ever
// signal. Pia builds its wakeup out of this: it parks on the event fd while a session comes up
// and moves only when something writes to it, so a stub here is a game that waits for ever.
//
// Backed by a datagram socket connected to itself: writing makes it readable, poll() sees it
// like any other socket, and none of the surrounding plumbing has to learn about a second kind
// of descriptor. The counter semantics of a real eventfd are approximated by one datagram per
// write, which is all a wakeup needs.
void BSD_USA::EventFd(HLERequestContext& ctx) {
    // EventFd(nn::socket::EventFdFlags flags, u64 initval). Flags come first, then four bytes of
    // padding before the 64-bit initial value -- read the other way round the counter starts at
    // whatever the flags were and the flags are lost, so every event fd a title creates is born
    // already signalled and its poller is woken for work that was never queued.
    IPC::RequestParser rp{ctx};
    const u32 flags = rp.Pop<u32>();
    rp.Pop<u32>(); // padding
    const u64 initval = rp.Pop<u64>();

    LOG_DEBUG(Service, "called. flags={}, initval={}", flags, initval);

    const s32 fd = FindFreeFileDescriptorHandle();
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        BuildErrnoResponse(ctx, Errno::MFILE);
        return;
    }

    auto socket = std::make_shared<Network::Socket>();
    socket->Initialize(Network::Domain::INET, Network::Type::DGRAM, Network::Protocol::UDP);

    // Loopback, kernel-chosen port, then connected to whatever it was given: a self-pipe.
    Network::SockAddrIn loopback{};
    loopback.family = Network::Domain::INET;
    loopback.ip = {127, 0, 0, 1};
    loopback.portno = 0;

    if (socket->Bind(loopback) != Network::Errno::SUCCESS) {
        LOG_ERROR(Service, "Could not bind an event fd");
        BuildErrnoResponse(ctx, Errno::INVAL);
        return;
    }

    const auto [local, local_errno] = socket->GetSockName();
    if (local_errno != Network::Errno::SUCCESS ||
        socket->Connect(local) != Network::Errno::SUCCESS) {
        LOG_ERROR(Service, "Could not connect an event fd to itself");
        BuildErrnoResponse(ctx, Errno::INVAL);
        return;
    }

    socket->SetNonBlock(true);

    file_descriptors[fd] = FileDescriptor{};
    FileDescriptor& descriptor = *file_descriptors[fd];
    descriptor.socket = socket;
    descriptor.is_connection_based = false;
    descriptor.flags = Network::FLAG_O_NONBLOCK;

    descriptor.event_value = std::make_shared<std::atomic<u64>>(initval);


    IPC::ResponseBuilder rb{ctx, 4};
    rb.Push(ResultSuccess);
    rb.Push<s32>(fd);
    rb.PushEnum(Errno::SUCCESS);
}

template <typename Work>
void BSD_USA::ExecuteWork(HLERequestContext& ctx, Work work) {
    work.Execute(this);
    work.Response(ctx);
}

std::pair<s32, Errno> BSD_USA::SocketImpl(Domain domain, Type type, Protocol protocol) {
    // user bsd:u has restrictions on SOCK_SEQPACKET and SOCK_RAW
    if (is_user && (type == Type::SEQPACKET || type == Type::RAW)) {
        if (type == Type::RAW && domain == Domain::INET && protocol == Protocol::ICMP) {
            // fine, can use on bsd:s and bsd:u
        } else {
            return {-1, Errno::INVAL};
        }
    }

    [[maybe_unused]] const bool unk_flag = (static_cast<u32>(type) & 0x20000000) != 0;
    UNIMPLEMENTED_IF_MSG(unk_flag, "Unknown flag in type");
    type = static_cast<Type>(static_cast<u32>(type) & ~0x20000000);

    // [OpenPak] SOCK_CLOEXEC was never stripped here, so a guest that set it handed Translate() a
    // type of 0x10000000 and the socket could not be created at all. It has no meaning for an
    // emulated descriptor table, but it must not survive into the base type.
    type = static_cast<Type>(static_cast<u32>(type) & ~0x10000000);

    // [OpenPak] With the flags gone some titles are left asking for Type::Unspecified, which
    // Translate() cannot map either. Risk of Rain 2 asks for exactly that -- SOCK_CLOEXEC over a
    // base type of 0 -- right after resolving localhost, and on Ryujinx the failed socket cost the
    // emulator a null dereference ~105 ms later, four runs running, while a real Switch played on.
    // Mapping it to STREAM is what got Ryujinx into an RoR2 lobby (2026-09-26, measured). It cannot
    // regress a working title: the alternative for Unspecified is a call that always fails.
    if (type == Type::Unspecified) {
        LOG_INFO(Service, "socket type 0 -> STREAM (base type unset by the guest)");
        type = Type::STREAM;
    }

    const s32 fd = FindFreeFileDescriptorHandle();
    if (fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    file_descriptors[fd] = FileDescriptor{};
    FileDescriptor& descriptor = *file_descriptors[fd];
    // ENONMEM might be thrown here

    LOG_INFO(Service, "New socket fd={}", fd);

    auto room_member = Network::GetRoomMember().lock();
    if (room_member && room_member->IsConnected()) {
        descriptor.socket = std::make_shared<Network::ProxySocket>();
    } else {
        descriptor.socket = std::make_shared<Network::Socket>();
    }

    // [OpenPak] Initialize's Errno must not be dropped. A guest that tests `fd < 0` reads the
    // descriptor we would otherwise hand back as a perfectly good socket, then calls methods on one
    // that was never opened. Measured on Ryujinx with Risk of Rain 2 (2026-09-26), which asks for
    // socket type 0x10000000 -- SOCK_CLOEXEC over a base type of 0: the host rejects it, and
    // ~100 ms after the emulator reported success the title dereferenced null and died. Eden and
    // Citron both reached the same end by a shorter road, never having looked at the result.
    const Network::Errno init_errno =
        descriptor.socket->Initialize(Translate(domain), Translate(type), Translate(protocol));
    if (init_errno != Network::Errno::SUCCESS) {
        LOG_ERROR(Service, "Socket creation failed for fd={} domain={} type={} protocol={}", fd,
                  domain, type, protocol);
        file_descriptors[fd].reset();
        return {-1, Translate(init_errno)};
    }
    descriptor.is_connection_based = IsConnectionBased(type);

    if (Settings::values.airplane_mode.GetValue() && descriptor.is_connection_based) {
        LOG_ERROR(Service, "Airplane mode is enabled, cannot create socket");
        return {-1, Errno::NOTCONN};
    }

    return {fd, Errno::SUCCESS};
}

bool BSD_USA::PollSetIncludesEventFd(std::span<const u8> read_buffer, s32 nfds) const {
    if (nfds <= 0 || read_buffer.size() < static_cast<size_t>(nfds) * sizeof(PollFD)) {
        return false;
    }
    std::vector<PollFD> fds(nfds);
    std::memcpy(fds.data(), read_buffer.data(), nfds * sizeof(PollFD));
    for (const PollFD& pollfd : fds) {
        if (pollfd.fd < 0 || pollfd.fd > static_cast<s32>(MAX_FD)) {
            continue;
        }
        const auto& descriptor = file_descriptors[pollfd.fd];
        if (descriptor && descriptor->event_value) {
            return true;
        }
    }
    return false;
}

std::pair<s32, Errno> BSD_USA::PollImpl(std::vector<u8>& write_buffer, std::span<const u8> read_buffer,
                                    s32 nfds, s32 timeout) {
    if (nfds <= 0) {
        // When no entries are provided, -1 is returned with errno zero
        return {-1, Errno::SUCCESS};
    }
    if (read_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }
    if (write_buffer.size() < nfds * sizeof(PollFD)) {
        return {-1, Errno::INVAL};
    }

    std::vector<PollFD> fds(nfds);
    std::memcpy(fds.data(), read_buffer.data(), nfds * sizeof(PollFD));

    if (timeout >= 0) {
        const s64 seconds = timeout / 1000;
        const u64 nanoseconds = 1'000'000 * (static_cast<u64>(timeout) % 1000);

        if (seconds < 0) {
            return {-1, Errno::INVAL};
        }
        if (nanoseconds > 999'999'999) {
            return {-1, Errno::INVAL};
        }
    } else if (timeout != -1) {
        return {-1, Errno::INVAL};
    }

    for (PollFD& pollfd : fds) {
        ASSERT(False(pollfd.revents));

        if (pollfd.fd > static_cast<s32>(MAX_FD) || pollfd.fd < 0) {
            LOG_ERROR(Service, "File descriptor handle={} is invalid", pollfd.fd);
            pollfd.revents = PollEvents{};
            return {0, Errno::SUCCESS};
        }

        const std::optional<FileDescriptor>& descriptor = file_descriptors[pollfd.fd];
        if (!descriptor) {
            LOG_TRACE(Service, "File descriptor handle={} is not allocated", pollfd.fd);
            pollfd.revents = PollEvents::Nval;
            return {0, Errno::SUCCESS};
        }
    }

    {
        std::string asked;
        for (const PollFD& pollfd : fds) {
            asked += fmt::format(" {}:{:#x}", pollfd.fd, static_cast<u16>(pollfd.events));
        }
        LOG_DEBUG(Service, "Poll asking{} timeout={}", asked, timeout);
        // [OpenPak] The NPLN SDK re-verifies the returned array against a mask cached in its
        // TLS; a round-trip that alters the bytes makes it take its error path and stall.
        // Log the returned array verbatim so a freeze can be compared against the request.
        std::string returned;
        for (const PollFD& pollfd : fds) {
            returned += fmt::format(" fd={} e={:#x} r={:#x} |", pollfd.fd,
                                    static_cast<u16>(pollfd.events),
                                    static_cast<u16>(pollfd.revents));
        }
        LOG_DEBUG(Service, "Poll answered:{} (timeout={})", returned, timeout);
    }

    // [OpenPak] An event fd is answered from its counter, never from the host. It used to carry
    // a byte on a loopback socket purely to make host poll() say "readable", with the counter
    // kept alongside -- and the two drift apart in both directions: a send that fails raises the
    // counter with no byte, so the wakeup is lost for good, and a read of a zero counter left a
    // stale byte behind, so poll() reported readable forever. A lost wakeup is a title that
    // queued work and never wrote it: gRPC kicks its poller this way, which is why NPLN sat on
    // an established connection without ever opening a stream. The counter is the whole truth.
    std::vector<Network::PollFD> host_pollfds;
    std::vector<size_t> host_of(fds.size(), std::numeric_limits<size_t>::max());
    s32 events_ready = 0;

    for (size_t i = 0; i < fds.size(); ++i) {
        const FileDescriptor& descriptor = *file_descriptors[fds[i].fd];

        if (descriptor.event_value) {
            // A title's gRPC stack polls its wakeup event fd with a zero event mask -- valid
            // POSIX, "only tell me about errors" -- yet expects readability to be reported once
            // it writes that event fd. Taking the zero mask literally parks gRPC's wakeup loop
            // forever, and queued work (the first NPLN RPC above all) never runs.
            const bool wants_in = True(fds[i].events & PollEvents::In) ||
                                  fds[i].events == PollEvents{};
            const bool readable = descriptor.event_value->load() > 0 && wants_in;
            fds[i].revents = readable ? PollEvents::In : PollEvents{};
            if (readable) {
                ++events_ready;
                // Poll only observes readiness. The subsequent event fd read consumes the
                // counter; clearing it here makes that read fail with AGAIN and loses the wakeup.
            }
            continue;
        }

        host_of[i] = host_pollfds.size();

        Network::PollFD& host = host_pollfds.emplace_back();
        host.socket = descriptor.socket.get();
        host.events = Translate(fds[i].events);
        // [OpenPak] A zero-mask poll entry is still answered when the descriptor has activity:
        // the title's gRPC stack parks a poll of [wakeup eventfd: In, channel socket: 0] and
        // relies on the socket's readiness surfacing through that same wait. Linux poll() with
        // events=0 only ever reports ERR/HUP, so ask the host for In|Out and gate the reported
        // revents back to the guest by what it actually has.
        if (host.events == Network::PollEvents{}) {
            host.events = Network::PollEvents::In | Network::PollEvents::Out;
        }
        host.revents = Network::PollEvents{};
    }

    // [OpenPak] A poll that was asked to wait forever waits forever. It is served in slices only
    // so that shutdown stays responsive -- the slice is never reported to the guest as a result.
    // Answering "nothing ready" to a caller that asked for -1 is a lie poll(2) never tells, and a
    // gRPC title believes it: the transport treats the wakeup as spurious, leaves its queued work
    // queued, and the connection sits established without ever opening a stream.
    constexpr s32 InfinitePollSliceMs = 250;

    // Re-read the event fd counters. They are answered from the counter rather than by the host,
    // so a wait that only re-polls the host descriptors would never notice one being signalled --
    // and the eventfd is precisely how a gRPC poller is woken.
    const auto recheck_events = [&fds, &events_ready]() {
        events_ready = 0;
        for (PollFD& pollfd : fds) {
            const FileDescriptor& descriptor = *file_descriptors[pollfd.fd];
            if (!descriptor.event_value) {
                continue;
            }
            const bool wants_in = True(pollfd.events & PollEvents::In) ||
                                  pollfd.events == PollEvents{};
            const bool readable = descriptor.event_value->load() > 0 && wants_in;
            pollfd.revents = readable ? PollEvents::In : PollEvents{};
            if (readable) {
                ++events_ready;
            }
        }
    };

    auto result = Network::Poll(host_pollfds, events_ready > 0 ? 0 : (timeout < 0 ? InfinitePollSliceMs : timeout));

    if (timeout < 0 && events_ready == 0 && result.first == 0) {
        recheck_events();
    }

    // A slice of an infinite poll that found nothing answers ETIMEDOUT, not success. Hardware's
    // poller is told its wait expired and goes and runs whatever was waiting on a timer -- which
    // for a gRPC transport is the queued RPC. Answered as plain success with no events it reads
    // as a spurious wakeup instead, the queue is left alone, and the connection sits established
    // without ever opening a stream. Ryujinx answers ETIMEDOUT here and the same title works.
    bool timed_out_waiting = false;
    if (timeout < 0 && events_ready == 0 && result.first == 0 &&
        result.second == Network::Errno::SUCCESS) {
        timed_out_waiting = true;
    }

    for (size_t i = 0; i < fds.size(); ++i) {
        if (host_of[i] != std::numeric_limits<size_t>::max()) {
            fds[i].revents = Translate(host_pollfds[host_of[i]].revents);
        }
        if (True(fds[i].revents)) {
            LOG_DEBUG(Service, "Poll fd={} events={:#x} -> revents={:#x}", fds[i].fd,
                      static_cast<u16>(fds[i].events), static_cast<u16>(fds[i].revents));
        } else {
            LOG_TRACE(Service, "Poll fd={} events={:#x} -> nothing", fds[i].fd,
                      static_cast<u16>(fds[i].events));
        }
    }
    std::memcpy(write_buffer.data(), fds.data(), nfds * sizeof(PollFD));

    auto [ready, bsd_errno] = Translate(result);

    // Event fds were answered here, not by the host, so they count here too -- and a poll that
    // found one is a success however the host poll of the rest turned out.
    if (events_ready > 0) {
        ready = (ready > 0 ? ready : 0) + events_ready;
        bsd_errno = Errno::SUCCESS;
    } else if (timed_out_waiting) {
        ready = 0;
        bsd_errno = Errno::TIMEDOUT;
    }

    return {ready, bsd_errno};
}

std::pair<s32, Errno> BSD_USA::AcceptImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    const s32 new_fd = FindFreeFileDescriptorHandle();
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return {-1, Errno::MFILE};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];
    auto [result, bsd_errno] = descriptor.socket->Accept();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return {-1, Translate(bsd_errno)};
    }

    file_descriptors[new_fd] = FileDescriptor{};
    FileDescriptor& new_descriptor = *file_descriptors[new_fd];
    new_descriptor.socket = std::move(result.socket);
    new_descriptor.is_connection_based = descriptor.is_connection_based;

    const SockAddrIn guest_addr_in = Translate(result.sockaddr_in);
    PutValue(write_buffer, guest_addr_in);

    return {new_fd, Errno::SUCCESS};
}

Errno BSD_USA::BindImpl(s32 fd, std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    ASSERT(addr.size() >= 16);
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    auto addr_in = GetValue<SockAddrIn>(addr);

    return Translate(file_descriptors[fd]->socket->Bind(Translate(addr_in)));
}

/// [OpenPak] Whether this address is the OpenPak server itself: the address every redirected
/// name resolves to, in either family's spelling. Those are ours, they are TCP, and they are
/// the ones a title's gRPC stack (NPLN above all) dials.
static bool OpenPakServerTarget(const Network::SockAddrIn& addr) {
    if (!Settings::values.enable_openpak.GetValue()) {
        return false;
    }

    std::string ip = Settings::values.openpak_server_ip.GetValue();
    if (ip.empty()) {
        if (const char* env = std::getenv("OPENPAK_SERVER_IP"); env != nullptr && *env != '\0') {
            ip = env;
        }
    }
    if (ip.empty()) {
        return false;
    }

    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }

    return addr.ip[0] == (a & 0xff) && addr.ip[1] == (b & 0xff) && addr.ip[2] == (c & 0xff) &&
           addr.ip[3] == (d & 0xff);
}

/// [OpenPak] A title's gRPC stack creates its first call while the channel is still connecting
/// and then polls the socket with a zero event mask -- which can never report POLLOUT -- so a
/// connect(2) answered EINPROGRESS leaves the completion undeliverable and the title waits
/// forever. Complete the connect synchronously instead: wait out the host's ~10 ms connect and
/// answer SUCCESS, which is what the working Ryujinx build does (its
/// NEXTENDO_GRPC_CONNECT_SYNC repair) and what the title's own poll then reads as an already-
/// established socket. Scoped to connects aimed at the OpenPak server, and only when the wait
/// actually completes within the budget; everything else keeps EINPROGRESS.
static Errno CompleteConnectSync(Network::SocketBase& socket,
                                 const Network::SockAddrIn& addr, Errno result) {
    if (result != Errno::INPROGRESS || !OpenPakServerTarget(addr)) {
        return result;
    }

    const auto fd = socket.GetFD();
    bool writable = false;
#ifdef _WIN32
    WSAPOLLFD pollfd{};
    pollfd.fd = fd;
    pollfd.events = POLLOUT;
    writable = WSAPoll(&pollfd, 1, 2000) == 1 && (pollfd.revents & (POLLOUT | POLLERR | POLLHUP));
#else
    pollfd pollfd{};
    pollfd.fd = fd;
    pollfd.events = POLLOUT;
    writable = ::poll(&pollfd, 1, 2000) == 1 && (pollfd.revents & (POLLOUT | POLLERR | POLLHUP));
#endif
    if (!writable) {
        return result;
    }

    const auto [pending_err, getsockopt_err] = socket.GetPendingError();
    if (getsockopt_err != Network::Errno::SUCCESS || pending_err != Network::Errno::SUCCESS) {
        return result;
    }

    LOG_INFO(Service,
             "[OpenPak] Connect to the OpenPak server completed synchronously; the channel is "
             "READY before the title's first call");
    return Errno::SUCCESS;
}

Errno BSD_USA::ConnectImpl(s32 fd, std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }

    ASSERT(addr.size() >= 16);
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    // [OpenPak] An IPv6 sockaddr arrives as {len, family=28, port, flowinfo, addr[16], scope}:
    // a title's gRPC stack (NPLN) dials its dual-mode socket with the v4-mapped form of the
    // resolver's IPv4 answer, so the address this layer speaks is the mapped tail. The socket
    // itself -- created AF_INET6 -- decides the on-wire shape it is given.
    if (addr.size() >= 24 && addr[1] == 28) {
        Network::SockAddrIn mapped{};
        mapped.family = Network::Domain::INET6;
        mapped.portno = static_cast<u16>(addr[2] << 8 | addr[3]);
        std::memcpy(mapped.ip.data(), addr.data() + 20, mapped.ip.size());

        Errno result = Translate(file_descriptors[fd]->socket->Connect(mapped));
        result = CompleteConnectSync(*file_descriptors[fd]->socket, mapped, result);

        LOG_DEBUG(Service,
                  "[OpenPak] Connect fd={} -> [v6 mapped] {}.{}.{}.{}:{} -> errno {}", fd,
                  mapped.ip[0], mapped.ip[1], mapped.ip[2], mapped.ip[3], mapped.portno,
                  static_cast<u32>(result));

        if (result == Errno::ISCONN) {
            return Errno::SUCCESS;
        }
        return result;
    }

    auto addr_in = GetValue<SockAddrIn>(addr);

    Errno result = Translate(file_descriptors[fd]->socket->Connect(Translate(addr_in)));
    result = CompleteConnectSync(*file_descriptors[fd]->socket, Translate(addr_in), result);

    // [OpenPak] Where a connection went and whether it took: a title dialling the wrong address
    // and one dialling the right address and being refused look identical without this.
    const auto translated = Translate(addr_in);
    LOG_DEBUG(Service,
              "[OpenPak] Connect fd={} -> {}.{}.{}.{}: guest port field {} -> dialled {} "
              "(bytes {:02x} {:02x} {:02x} {:02x}) -> errno {}",
              fd, addr_in.ip[0], addr_in.ip[1], addr_in.ip[2], addr_in.ip[3], addr_in.portno,
              translated.portno, addr[0], addr[1], addr[2], addr[3],
              static_cast<u32>(result));

    if (result == Errno::ISCONN) {
        LOG_DEBUG(Service, "returned ISCONN - socket already connected");
        return Errno::SUCCESS;
    }

    return result;
}

Errno BSD_USA::GetPeerNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }

    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    const auto [addr_in, bsd_errno] = file_descriptors[fd]->socket->GetPeerName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    ASSERT(write_buffer.size() >= sizeof(guest_addrin));
    write_buffer.resize(sizeof(guest_addrin));
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD_USA::GetSockNameImpl(s32 fd, std::vector<u8>& write_buffer) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }

    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    const auto [addr_in, bsd_errno] = file_descriptors[fd]->socket->GetSockName();
    if (bsd_errno != Network::Errno::SUCCESS) {
        return Translate(bsd_errno);
    }
    const SockAddrIn guest_addrin = Translate(addr_in);

    ASSERT(write_buffer.size() >= sizeof(guest_addrin));
    write_buffer.resize(sizeof(guest_addrin));
    PutValue(write_buffer, guest_addrin);
    return Translate(bsd_errno);
}

Errno BSD_USA::ListenImpl(s32 fd, s32 backlog) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }
    return Translate(file_descriptors[fd]->socket->Listen(backlog));
}

std::pair<s32, Errno> BSD_USA::FcntlImpl(s32 fd, FcntlCmd cmd, s32 arg) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];

    switch (cmd) {
    case FcntlCmd::GETFL:
        ASSERT(arg == 0);
        return {descriptor.flags, Errno::SUCCESS};
    case FcntlCmd::SETFL: {
        const bool enable = (arg & Network::FLAG_O_NONBLOCK) != 0;
        const Errno bsd_errno = Translate(descriptor.socket->SetNonBlock(enable));
        if (bsd_errno != Errno::SUCCESS) {
            return {-1, bsd_errno};
        }
        descriptor.flags = arg;
        return {0, Errno::SUCCESS};
    }
    default:
        UNIMPLEMENTED_MSG("Unimplemented cmd={}", cmd);
        return {-1, Errno::SUCCESS};
    }
}

Errno BSD_USA::GetSockOptImpl(s32 fd, u32 level, OptName optname, std::vector<u8>& optval) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    // TCP option 1 is TCP_NODELAY, separate from the SOL_SOCKET options.
    if (level == static_cast<u32>(SocketLevel::TCP) && static_cast<u32>(optname) == 1) {
        if (optval.size() < sizeof(u32)) {
            return Errno::INVAL;
        }
        auto [value, error] = file_descriptors[fd]->socket->GetNoDelay();
        if (error == Network::Errno::SUCCESS) {
            optval.resize(sizeof(value));
            PutValue(optval, value);
        }
        return Translate(error);
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        // [OpenPak] The set side already tolerates any level it does not specifically implement;
        // this must do the same, or a title that sets an option at some other level and reads it
        // straight back to confirm sees a set-ok/get-fails mismatch and closes before connect.
        // Echo zeroed bytes of the requested size rather than failing outright.
        LOG_WARNING(Service, "(STUBBED) Unknown getsockopt level={}, echoing zeroed value", level);
        std::fill(optval.begin(), optval.end(), 0);
        return Errno::SUCCESS;
    }

    Network::SocketBase* const socket = file_descriptors[fd]->socket.get();

    const auto read_scalar = [&](auto getter) {
        if (optval.size() < sizeof(u32)) {
            return Errno::INVAL;
        }
        const auto [value, error] = (socket->*getter)();
        if (error == Network::Errno::SUCCESS) {
            optval.resize(sizeof(value));
            PutValue(optval, value);
        }
        return Translate(error);
    };

    switch (optname) {
    case OptName::REUSEADDR:
        return read_scalar(&Network::SocketBase::GetReuseAddr);
    case OptName::KEEPALIVE:
        return read_scalar(&Network::SocketBase::GetKeepAlive);
    case OptName::BROADCAST:
        return read_scalar(&Network::SocketBase::GetBroadcast);
    case OptName::SNDBUF:
        return read_scalar(&Network::SocketBase::GetSndBuf);
    case OptName::RCVBUF:
        return read_scalar(&Network::SocketBase::GetRcvBuf);
    case OptName::SNDTIMEO:
        return read_scalar(&Network::SocketBase::GetSndTimeo);
    case OptName::RCVTIMEO:
        return read_scalar(&Network::SocketBase::GetRcvTimeo);
    case OptName::TYPE:
        return read_scalar(&Network::SocketBase::GetSocketType);
    case OptName::LINGER: {
        // Two fields, so it does not go through read_scalar: a title reads back the struct it set.
        if (optval.size() < sizeof(Linger)) {
            return Errno::INVAL;
        }

        u32 seconds{};
        const auto [onoff, error] = socket->GetLinger(&seconds);

        if (error == Network::Errno::SUCCESS) {
            optval.resize(sizeof(Linger));
            PutValue(optval, Linger{.onoff = onoff, .linger = seconds});
        }

        return Translate(error);
    }
    case OptName::NOSIGPIPE:
        // Set is a no-op here, so the honest readback is the value a no-op leaves behind.
        if (optval.size() < sizeof(u32)) {
            return Errno::INVAL;
        }
        optval.resize(sizeof(u32));
        PutValue(optval, u32{0});
        return Errno::SUCCESS;
    case OptName::ERROR_: {
        auto [pending_err, getsockopt_err] = socket->GetPendingError();
        if (getsockopt_err == Network::Errno::SUCCESS) {
            Errno translated_pending_err = Translate(pending_err);
            ASSERT_OR_EXECUTE_MSG(
                optval.size() == sizeof(Errno), { return Errno::INVAL; },
                "Incorrect getsockopt option size");
            optval.resize(sizeof(Errno));
            PutValue(optval, translated_pending_err);

            // What this answers decides whether a title writes its first byte on a freshly
            // connected socket or gives up on it, so it is worth a line.
            LOG_INFO(Service, "[OpenPak] SO_ERROR read: {}", static_cast<u32>(translated_pending_err));
        }
        return Translate(getsockopt_err);
    }
    default: {
        // [OpenPak] Whatever the set side tolerated without applying is echoed back here: the
        // guest asked, so it gets its own bytes (or zeros) and SUCCESS -- never NOPROTOOPT,
        // which next to the tolerant set reads as a broken socket and kills the connection.
        // Nintendo's 0x80000001 linger-shaped option is the one NPLN's stack actually verifies.
        const u64 key = static_cast<u64>(level) << 32 | static_cast<u32>(optname);
        const auto& descriptor = *file_descriptors[fd];
        const auto stored = descriptor.feigned_sockopts.find(key);
        if (stored != descriptor.feigned_sockopts.end() && !stored->second.empty()) {
            optval.resize(std::min(optval.size(), stored->second.size()));
            std::copy(stored->second.begin(), stored->second.begin() + optval.size(),
                      optval.begin());
        } else {
            LOG_WARNING(Service, "Unimplemented getsockopt optname={:#x}, echoing zeroed value",
                        static_cast<u32>(optname));
            std::fill(optval.begin(), optval.end(), 0);
        }
        return Errno::SUCCESS;
    }
    }
}

Errno BSD_USA::SetSockOptImpl(s32 fd, u32 level, OptName optname, std::span<const u8> optval) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    if (level == static_cast<u32>(SocketLevel::TCP) && static_cast<u32>(optname) == 1) {
        if (optval.size() < sizeof(u32)) {
            return Errno::INVAL;
        }
        return Translate(file_descriptors[fd]->socket->SetNoDelay(GetValue<u32>(optval) != 0));
    }

    if (level != static_cast<u32>(SocketLevel::SOCKET)) {
        const u64 key = static_cast<u64>(level) << 32 | static_cast<u32>(optname);
        file_descriptors[fd]->feigned_sockopts[key].assign(optval.begin(), optval.end());
        LOG_WARNING(Service, "(STUBBED) setsockopt level={} optname={:#x} ({} bytes), feigned",
                    level, static_cast<u32>(optname), optval.size());
        return Errno::SUCCESS;
    }

    Network::SocketBase* const socket = file_descriptors[fd]->socket.get();

    // [OpenPak] Anything else this build does not know is feigned rather than force-fit into the
    // u32 options below: Nintendo's linger-shaped 0x80000001 carries eight bytes, and the get
    // side must be able to echo exactly what was set.
    switch (optname) {
    case OptName::REUSEADDR:
    case OptName::KEEPALIVE:
    case OptName::BROADCAST:
    case OptName::SNDBUF:
    case OptName::RCVBUF:
    case OptName::SNDTIMEO:
    case OptName::RCVTIMEO:
    case OptName::NOSIGPIPE:
    case OptName::LINGER:
        break;
    default: {
        const u64 key = static_cast<u64>(level) << 32 | static_cast<u32>(optname);
        file_descriptors[fd]->feigned_sockopts[key].assign(optval.begin(), optval.end());
        LOG_WARNING(Service, "(STUBBED) setsockopt optname={:#x} ({} bytes), feigned",
                    static_cast<u32>(optname), optval.size());
        return Errno::SUCCESS;
    }
    }

    if (optname == OptName::LINGER) {
        ASSERT(optval.size() == sizeof(Linger));
        auto linger = GetValue<Linger>(optval);
        ASSERT(linger.onoff == 0 || linger.onoff == 1);

        return Translate(socket->SetLinger(linger.onoff != 0, linger.linger));
    }

    ASSERT(optval.size() == sizeof(u32));
    auto value = GetValue<u32>(optval);

    switch (optname) {
    case OptName::REUSEADDR:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetReuseAddr(value != 0));
    case OptName::KEEPALIVE:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetKeepAlive(value != 0));
    case OptName::BROADCAST:
        ASSERT(value == 0 || value == 1);
        return Translate(socket->SetBroadcast(value != 0));
    case OptName::SNDBUF:
        return Translate(socket->SetSndBuf(value));
    case OptName::RCVBUF:
        return Translate(socket->SetRcvBuf(value));
    case OptName::SNDTIMEO:
        return Translate(socket->SetSndTimeo(value));
    case OptName::RCVTIMEO:
        return Translate(socket->SetRcvTimeo(value));
    case OptName::NOSIGPIPE:
        LOG_WARNING(Service, "(STUBBED) setting NOSIGPIPE to {}", value);
        return Errno::SUCCESS;
    default: {
        // [OpenPak] Tolerated, remembered, and echoed back by the matching get: an unimplemented
        // option must not answer SUCCESS here and NOPROTOOPT there, or a title verifying its own
        // settings abandons the socket (and the connection) over a disagreement we invented.
        const u64 key = static_cast<u64>(level) << 32 | static_cast<u32>(optname);
        file_descriptors[fd]->feigned_sockopts[key].assign(optval.begin(), optval.end());
        LOG_WARNING(Service, "(STUBBED) setsockopt level={} optname={:#x} ({} bytes), feigned",
                    level, static_cast<u32>(optname), optval.size());
        return Errno::SUCCESS;
    }
    }
}

Errno BSD_USA::ShutdownImpl(s32 fd, s32 how) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }
    const Network::ShutdownHow host_how = Translate(static_cast<ShutdownHow>(how));
    return Translate(file_descriptors[fd]->socket->Shutdown(host_how));
}

std::pair<s32, Errno> BSD_USA::RecvImpl(s32 fd, u32 flags, std::vector<u8>& message) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];

    // [OpenPak] An event fd answers with its count and clears it, in one read.
    if (descriptor.event_value) {
        const u64 value = descriptor.event_value->exchange(0);

        if (value == 0) {
            return {-1, Errno::AGAIN};
        }

        if (message.size() < sizeof(u64)) {
            return {-1, Errno::INVAL};
        }

        std::memcpy(message.data(), &value, sizeof(value));

        LOG_DEBUG(Service, "Event fd {} read {}", fd, value);

        return {static_cast<s32>(sizeof(u64)), Errno::SUCCESS};
    }

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
            descriptor.socket->SetNonBlock(true);
        }
    }

    const auto [ret, bsd_errno] = Translate(descriptor.socket->Recv(flags, message));

    // Restore original state
    if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
        descriptor.socket->SetNonBlock(false);
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD_USA::RecvFromImpl(s32 fd, u32 flags, std::vector<u8>& message,
                                        std::vector<u8>& addr) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    FileDescriptor& descriptor = *file_descriptors[fd];

    Network::SockAddrIn addr_in{};
    Network::SockAddrIn* p_addr_in = nullptr;
    if (descriptor.is_connection_based) {
        // Connection based file descriptors (e.g. TCP) zero addr
        addr.clear();
    } else {
        p_addr_in = &addr_in;
    }

    // Apply flags
    using Network::FLAG_MSG_DONTWAIT;
    using Network::FLAG_O_NONBLOCK;
    if ((flags & FLAG_MSG_DONTWAIT) != 0) {
        flags &= ~FLAG_MSG_DONTWAIT;
        if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
            descriptor.socket->SetNonBlock(true);
        }
    }

    auto [ret, bsd_errno] = Translate(descriptor.socket->RecvFrom(flags, message, p_addr_in));

    // [OpenPak] One unreachable peer must not read as the network dropping. A datagram socket
    // shared across peers -- which is what a NAT check and every P2P session use -- collects an
    // ICMP error from any of them, and the next read returns that error instead of the datagram
    // waiting behind it. Take the next one instead.
    if (!descriptor.is_connection_based) {
        for (int attempt = 0; attempt < 16 && (bsd_errno == Errno::CONNREFUSED ||
                                               bsd_errno == Errno::CONNRESET); ++attempt) {
            LOG_DEBUG(Service, "Discarding queued ICMP error on fd={} errno={}", fd,
                      static_cast<int>(bsd_errno));
            std::tie(ret, bsd_errno) =
                Translate(descriptor.socket->RecvFrom(flags, message, p_addr_in));
        }
    }

    // Restore original state
    if ((descriptor.flags & FLAG_O_NONBLOCK) == 0) {
        descriptor.socket->SetNonBlock(false);
    }

    // [OpenPak] Every datagram that arrives, with its source: a reply that never reaches the
    // guest and a reply that was never sent look identical without this.
    if (p_addr_in != nullptr && ret > 0) {
        LOG_DEBUG(Service, "RecvFrom fd={} <- {}:{} len={}", fd,
                  Network::IPv4AddressToString(p_addr_in->ip), p_addr_in->portno, ret);
    }

    // [OpenPak] The NAT check's answer is 16 bytes: [type][external port][external ip][server ip].
    // It is the only place this console is told how the outside world sees it, and a station
    // address built from the private LAN address instead is one nobody can dial.
    if (p_addr_in != nullptr && ret == 16 &&
        (p_addr_in->portno == 10025 || p_addr_in->portno == 10125)) {
        LOG_INFO(Service, "[OpenPak] NAT check: external address {}.{}.{}.{} (from {}:{})",
                 message[8], message[9], message[10], message[11],
                 Network::IPv4AddressToString(p_addr_in->ip), p_addr_in->portno);
    }

    if (p_addr_in) {
        if (ret < 0) {
            addr.clear();
        } else {
            ASSERT(addr.size() >= 16);
            const SockAddrIn result = Translate(addr_in);
            PutValue(addr, result);
        }
    }

    return {ret, bsd_errno};
}

std::pair<s32, Errno> BSD_USA::SendImpl(s32 fd, u32 flags, std::span<const u8> message) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }

    // [OpenPak] Writing an event fd adds to its count and wakes whoever is polling it.
    if (file_descriptors[fd]->event_value) {
        if (message.size() < sizeof(u64)) {
            return {-1, Errno::INVAL};
        }

        u64 value{};
        std::memcpy(&value, message.data(), sizeof(value));

        const u64 previous = file_descriptors[fd]->event_value->fetch_add(value);

        LOG_DEBUG(Service, "Event fd {} written {} (now {})", fd, value, previous + value);

        // [OpenPak] Wake any Poll() this title deferred waiting on this event fd: the deferral
        // event is what tells ServerManager to re-run the deferred handler, and this write is
        // the moment the wait it was holding out for has ended.
        if (Kernel::KEvent* deferral_event = GetBsdDeferralEvent()) {
            deferral_event->Signal(system.Kernel());
        }

        return {static_cast<s32>(sizeof(u64)), Errno::SUCCESS};
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return {-1, Errno::BADF};
    }
    return Translate(file_descriptors[fd]->socket->Send(message, flags));
}

std::pair<s32, Errno> BSD_USA::SendToImpl(s32 fd, u32 flags, std::span<const u8> message,
                                      std::span<const u8> addr) {
    if (!IsFileDescriptorValid(fd)) {
        return {-1, Errno::BADF};
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return {-1, Errno::BADF};
    }

    Network::SockAddrIn addr_in;
    Network::SockAddrIn* p_addr_in = nullptr;
    if (!addr.empty()) {
        ASSERT(addr.size() >= 16);
        auto guest_addr_in = GetValue<SockAddrIn>(addr);
        addr_in = Translate(guest_addr_in);
        p_addr_in = &addr_in;
    }

    const auto result = Translate(file_descriptors[fd]->socket->SendTo(flags, message, p_addr_in));

    // [OpenPak] Where a datagram went, which is the one thing a NAT check's log has to show:
    // probes that leave for the wrong address look exactly like probes nobody answered.
    if (p_addr_in != nullptr) {
        LOG_DEBUG(Service, "SendTo fd={} -> {}:{} len={} ret={}", fd,
                  Network::IPv4AddressToString(p_addr_in->ip), p_addr_in->portno, message.size(),
                  result.first);
    }

    return result;
}

Errno BSD_USA::CloseImpl(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return Errno::BADF;
    }

    const Errno bsd_errno = Translate(file_descriptors[fd]->socket->Close());
    if (bsd_errno != Errno::SUCCESS) {
        return bsd_errno;
    }

    LOG_INFO(Service, "Close socket fd={}", fd);

    file_descriptors[fd].reset();
    return bsd_errno;
}

std::variant<s32, Errno> BSD_USA::DuplicateSocketImpl(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return Errno::BADF;
    }

    const s32 new_fd = FindFreeFileDescriptorHandle();
    if (new_fd < 0) {
        LOG_ERROR(Service, "No more file descriptors available");
        return Errno::MFILE;
    }

    file_descriptors[new_fd] = FileDescriptor{
        .socket = file_descriptors[fd]->socket,
        .flags = file_descriptors[fd]->flags,
        .is_connection_based = file_descriptors[fd]->is_connection_based,
    };
    return new_fd;
}

std::optional<std::shared_ptr<Network::SocketBase>> BSD_USA::GetSocket(s32 fd) {
    if (!IsFileDescriptorValid(fd)) {
        return std::nullopt;
    }
    if (!file_descriptors[fd]->socket) {
        LOG_WARNING(Service, "Uninitialized socket");
        return std::nullopt;
    }
    return file_descriptors[fd]->socket;
}

s32 BSD_USA::FindFreeFileDescriptorHandle() noexcept {
    for (s32 fd = 0; fd < static_cast<s32>(file_descriptors.size()); ++fd) {
        if (!file_descriptors[fd]) {
            return fd;
        }
    }
    return -1;
}

bool BSD_USA::IsFileDescriptorValid(s32 fd) const noexcept {
    if (fd > static_cast<s32>(MAX_FD) || fd < 0) {
        LOG_ERROR(Service, "Invalid file descriptor handle={}", fd);
        return false;
    }
    if (!file_descriptors[fd]) {
        LOG_ERROR(Service, "File descriptor handle={} is not allocated", fd);
        return false;
    }
    return true;
}

void BSD_USA::BuildErrnoResponse(HLERequestContext& ctx, Errno bsd_errno) const noexcept {
    IPC::ResponseBuilder rb{ctx, 4};

    rb.Push(ResultSuccess);
    rb.Push<s32>(bsd_errno == Errno::SUCCESS ? 0 : -1);
    rb.PushEnum(bsd_errno);
}

void BSD_USA::OnProxyPacketReceived(const Network::ProxyPacket& packet) {
    for (auto& optional_descriptor : file_descriptors) {
        if (!optional_descriptor.has_value()) {
            continue;
        }
        FileDescriptor& descriptor = *optional_descriptor;
        descriptor.socket.get()->HandleProxyPacket(packet);
    }
}

BSD_USA::BSD_USA(Core::System& system_, const char* name, bool is_user_)
    : ServiceFramework{system_, name}
    , is_user{is_user_} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &BSD_USA::RegisterClient, "RegisterClient"},
        {1, &BSD_USA::StartMonitoring, "StartMonitoring"},
        {2, &BSD_USA::Socket, "Socket"},
        {3, &BSD_USA::SocketExempt, "SocketExempt"},
        {4, nullptr, "Open"},
        {5, &BSD_USA::Select, "Select"},
        {6, &BSD_USA::Poll, "Poll"},
        {7, &BSD_USA::Sysctl, "Sysctl"},
        {8, &BSD_USA::Recv, "Recv"},
        {9, &BSD_USA::RecvFrom, "RecvFrom"},
        {10, &BSD_USA::Send, "Send"},
        {11, &BSD_USA::SendTo, "SendTo"},
        {12, &BSD_USA::Accept, "Accept"},
        {13, &BSD_USA::Bind, "Bind"},
        {14, &BSD_USA::Connect, "Connect"},
        {15, &BSD_USA::GetPeerName, "GetPeerName"},
        {16, &BSD_USA::GetSockName, "GetSockName"},
        {17, &BSD_USA::GetSockOpt, "GetSockOpt"},
        {18, &BSD_USA::Listen, "Listen"},
        {19, nullptr, "Ioctl"},
        {20, &BSD_USA::Fcntl, "Fcntl"},
        {21, &BSD_USA::SetSockOpt, "SetSockOpt"},
        {22, &BSD_USA::Shutdown, "Shutdown"},
        {23, nullptr, "ShutdownAllSockets"},
        {24, &BSD_USA::Write, "Write"},
        {25, &BSD_USA::Read, "Read"},
        {26, &BSD_USA::Close, "Close"},
        {27, &BSD_USA::DuplicateSocket, "DuplicateSocket"},
        {28, nullptr, "GetResourceStatistics"},
        {29, &BSD_USA::RecvMMsg, "RecvMMsg"}, //3.0.0+
        {30, &BSD_USA::SendMMsg, "SendMMsg"}, //3.0.0+
        {31, &BSD_USA::EventFd, "EventFd"}, //7.0.0+
        {32, nullptr, "RegisterResourceStatisticsName"}, //7.0.0+
        {33, nullptr, "RegisterClientShared"}, //10.0.0+
        {34, nullptr, "GetSocketStatistics"}, //15.0.0+
        {35, nullptr, "NifIoctl"}, //17.0.0+
        {36, nullptr, "Unknown36"}, //18.0.0+
        {37, nullptr, "Unknown37"}, //18.0.0+
        {38, nullptr, "Unknown38"}, //18.0.0+
        {39, nullptr, "Unknown39"}, //20.0.0+
        {40, nullptr, "Unknown40"}, //20.0.0+
        {41, nullptr, "Unknown41"}, //21.0.0+
        {42, nullptr, "Unknown42"}, //21.0.0+
        {43, nullptr, "Unknown43"}, //21.0.0+
        {200, nullptr, "SetThreadCoreMask"}, //15.0.0+
        {201, nullptr, "GetThreadCoreMask"}, //15.0.0+
    };
    // clang-format on

    RegisterHandlers(functions);

    if (auto room_member = Network::GetRoomMember().lock()) {
        proxy_packet_received = room_member->BindOnProxyPacketReceived(
            [this](const Network::ProxyPacket& packet) { OnProxyPacketReceived(packet); });
    } else {
        LOG_ERROR(Service, "Network isn't initialized");
    }
}

BSD_USA::~BSD_USA() {
    if (auto room_member = Network::GetRoomMember().lock()) {
        room_member->Unbind(proxy_packet_received);
    }
}

std::unique_lock<std::mutex> BSD_USA::LockService() noexcept {
    return {};
}

BSDCFG::BSDCFG(Core::System& system_, const char *name)
    : ServiceFramework{system_, name} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "SetIfUp"},
        {1, nullptr, "SetIfUpWithEvent"},
        {2, nullptr, "CancelIf"},
        {3, nullptr, "SetIfDown"},
        {4, nullptr, "GetIfState"},
        {5, nullptr, "DhcpRenew"},
        {6, nullptr, "AddStaticArpEntry"},
        {7, nullptr, "RemoveArpEntry"},
        {8, nullptr, "LookupArpEntry"},
        {9, nullptr, "LookupArpEntry2"},
        {10, nullptr, "ClearArpEntries"},
        {11, nullptr, "ClearArpEntries2"},
        {12, nullptr, "PrintArpEntries"},
        {13, nullptr, "Unknown13"},
        {14, nullptr, "Unknown14"},
        {15, nullptr, "Unknown15"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

BSDCFG::~BSDCFG() = default;

BSD_NU::BSD_NU(Core::System& system_)
    : ServiceFramework{system_, "bsd:nu"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, nullptr, "CreateUserService"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

BSD_NU::~BSD_NU() = default;

} // namespace Service::Sockets
