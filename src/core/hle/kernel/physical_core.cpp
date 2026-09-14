// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/debugger/debugger.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc.h"
#include "core/memory.h"
#include <array>
#include <cstdlib>
#include <cstdio>

namespace Kernel {

// Temporary opt-in diagnostic: retain only the instructions immediately before thread 98 faults.
static const bool trace_fault = std::getenv("OPENPAK_TRACE_GUEST_FAULT") != nullptr;
static bool trace_armed = true;
static u64 watched_return = 0;
static u64 watched_value = 0;
static bool watched_saved = false;
static bool watched_reported = false;
static std::array<Kernel::Svc::ThreadContext, 128> fault_history{};
static size_t fault_steps = 0;

PhysicalCore::PhysicalCore(KernelCore& kernel, std::size_t core_index)
    : m_core_index{core_index}
{
    m_is_single_core = !kernel.IsMulticore();
}
PhysicalCore::~PhysicalCore() = default;

void PhysicalCore::RunThread(KernelCore& kernel, Kernel::KThread* thread) {
    auto* process = thread->GetOwnerProcess();
    auto& system = kernel.System();
    auto* interface = process->GetArmInterface(m_core_index);

    interface->Initialize();

    const auto EnterContext = [&]() {
        // Lock the core context.
        std::scoped_lock lk{m_guard};

        // Check if we are already interrupted. If we are, we can just stop immediately.
        if (m_is_interrupted) {
            return false;
        }

        // Mark that we are running.
        m_arm_interface = interface;
        m_current_thread = thread;

        // Acquire the lock on the thread parameters.
        // This allows us to force synchronization with Interrupt.
        interface->LockThread(thread);

        return true;
    };

    const auto ExitContext = [&]() {
        // Save the JIT context back to the thread so the debugger can
        // read the current register state. Debug halt paths (step,
        // breakpoint, watchpoint) return from RunThread without going
        // through the scheduler's Unload/SaveContext.
        if (system.DebuggerEnabled()) {
            interface->GetContext(thread->GetContext());
        }

        // Unlock the thread.
        interface->UnlockThread(thread);

        // Lock the core context.
        std::scoped_lock lk{m_guard};

        // On exit, we no longer are running.
        m_arm_interface = nullptr;
        m_current_thread = nullptr;
    };

    while (true) {
        // If the thread is scheduled for termination, exit.
        if (thread->HasDpc() && thread->IsTerminationRequested()) {
            thread->Exit(kernel);
        }

        // Notify the debugger and go to sleep if a step was performed
        // and this thread has been scheduled again.
        if (thread->GetStepState() == StepState::StepPerformed) {
            system.GetDebugger().NotifyThreadStopped(thread);
            thread->RequestSuspend(kernel, SuspendType::Debug);
            return;
        }

        // Otherwise, run the thread.
        Core::HaltReason hr{};
        {
            // If we were interrupted, exit immediately.
            if (!EnterContext()) {
                return;
            }

            if (trace_fault && trace_armed && thread->GetThreadId() == 98) {
                auto& c = fault_history[fault_steps++ % fault_history.size()];
                interface->GetContext(c);
                auto& memory = process->GetMemory();
                if (!watched_return && memory.Read32(c.pc) == 0xd106c3ff &&
                    memory.Read32(c.pc + 4) == 0xa91957fe) {
                    watched_return = c.sp - 0x20;
                    LOG_INFO(Core_ARM, "Watching callback return slot {:#x} at pc={:#x}", watched_return, c.pc);
                }
                if (watched_return && !watched_reported) {
                    const u64 value = memory.Read64(watched_return);
                    if (value != 0) { watched_value = value; watched_saved = true; }
                    if (watched_saved && value == 0) {
                        watched_reported = true;
                        if (auto* out = std::fopen("/tmp/eden-return-overwrite-trace.txt", "w")) {
                            for (size_t i = fault_steps > 128 ? fault_steps - 128 : 0; i < fault_steps; ++i) {
                                const auto& h = fault_history[i % 128];
                                std::fprintf(out, "pc=%llx lr=%llx sp=%llx x0=%llx x1=%llx x2=%llx x8=%llx\n",
                                             (unsigned long long)h.pc, (unsigned long long)h.lr,
                                             (unsigned long long)h.sp, (unsigned long long)h.r[0],
                                             (unsigned long long)h.r[1], (unsigned long long)h.r[2],
                                             (unsigned long long)h.r[8]);
                            }
                            std::fclose(out);
                        }
                        LOG_ERROR(Core_ARM, "Callback return changed from {:#x} to zero, next pc={:#x}", watched_value, c.pc);
                    }
                }
                hr = interface->StepThread(thread);
            } else if (thread->GetStepState() == StepState::StepPending) {
                hr = interface->StepThread(thread);

                if (True(hr & Core::HaltReason::StepThread)) {
                    thread->SetStepState(StepState::StepPerformed);
                }
            } else {
                hr = interface->RunThread(thread);
            }

            ExitContext();
        }

        // Determine why we stopped.
        // If a step completed successfully, skip other halt reason handlers
        // the step takes priority (e.g. step may also set InstructionBreakpoint
        // if the next instruction happens to be a breakpoint).
        const bool step_completed = True(hr & Core::HaltReason::StepThread)
                                    && thread->GetStepState() == StepState::StepPerformed;
        const bool supervisor_call = !step_completed && True(hr & Core::HaltReason::SupervisorCall);
        const bool prefetch_abort = !step_completed && True(hr & Core::HaltReason::PrefetchAbort);
        const bool breakpoint = !step_completed && True(hr & Core::HaltReason::InstructionBreakpoint);
        const bool data_abort = !step_completed && True(hr & Core::HaltReason::DataAbort);
        const bool interrupt = !step_completed && True(hr & Core::HaltReason::BreakLoop);

        // Since scheduling may occur here, we cannot use any cached
        // state after returning from calls we make.

        // Notify the debugger and go to sleep if a breakpoint was hit,
        // or if the thread is unable to continue for any reason.
        if (breakpoint || prefetch_abort) {
            if (prefetch_abort) {
                if (trace_fault && thread->GetThreadId() == 98) {
                    if (auto* out = std::fopen("/tmp/eden-guest-fault-trace.txt", "w")) {
                        const size_t start = fault_steps > fault_history.size()
                                                 ? fault_steps - fault_history.size() : 0;
                        for (size_t i = start; i < fault_steps; ++i) {
                            const auto& c = fault_history[i % fault_history.size()];
                            std::fprintf(out, "pc=%llx lr=%llx sp=%llx x0=%llx x8=%llx x19=%llx x20=%llx x21=%llx\n",
                                         (unsigned long long)c.pc, (unsigned long long)c.lr,
                                         (unsigned long long)c.sp, (unsigned long long)c.r[0],
                                         (unsigned long long)c.r[8], (unsigned long long)c.r[19],
                                         (unsigned long long)c.r[20], (unsigned long long)c.r[21]);
                        }
                        std::fclose(out);
                    }
                }
                LOG_ERROR(Core_ARM, "Guest execution fault: thread={} tls={:#x}",
                          thread->GetThreadId(), GetInteger(thread->GetTlsAddress()));
            }
            if (breakpoint) {
                interface->RewindBreakpointInstruction();
                // RewindBreakpointInstruction sets the JIT state to the
                // saved breakpoint context. Update the thread context to
                // match, since ExitContext already saved the post-execution
                // state.
                interface->GetContext(thread->GetContext());
            }
            if (system.DebuggerEnabled()) {
                system.GetDebugger().NotifyThreadStopped(thread);
            } else {
                interface->LogBacktrace(process);
            }
            thread->RequestSuspend(kernel, SuspendType::Debug);
            return;
        }

        // Notify the debugger and go to sleep on data abort.
        if (data_abort) {
            if (system.DebuggerEnabled()) {
                system.GetDebugger().NotifyThreadWatchpoint(thread, *interface->HaltedWatchpoint());
            }
            thread->RequestSuspend(kernel, SuspendType::Debug);
            return;
        }

        // Handle system calls.
        if (supervisor_call) {
            if (trace_fault && thread->GetThreadId() == 98 &&
                interface->GetSvcNumber() == 0x21) {
                const auto tls = GetInteger(thread->GetTlsAddress());
                for (u64 offset = 0; offset < 0x80; offset += 4) {
                    if (process->GetMemory().Read32(tls + offset) == 0x49434653 &&
                        process->GetMemory().Read32(tls + offset + 8) == 24) {
                        trace_armed = true;
                        LOG_INFO(Core_ARM, "Fault instruction recorder armed");
                        break;
                    }
                }
            }
            const auto trace_thread = thread->GetThreadId();
            if (trace_thread == 98) {
                Kernel::Svc::ThreadContext context{};
                interface->GetContext(context);
                LOG_INFO(Core_ARM,
                         "Trace thread={} svc={:#x} pc={:#x} lr={:#x} sp={:#x} x0={:#x} x1={:#x} "
                         "x2={:#x} x3={:#x}",
                         trace_thread, interface->GetSvcNumber(), context.pc, context.lr,
                         context.sp, context.r[0], context.r[1], context.r[2], context.r[3]);
                // [OpenPak] The NPLN worker's pollfd array: the SDK re-verifies it against a
                // mask cached in its TLS on every wait, and the mismatch is the freeze.
                if (interface->GetSvcNumber() == 0x1c) {
                    auto& memory = process->GetMemory();
                    std::string arr;
                    for (std::size_t i = 0; i < 4; ++i) {
                        const u32 word =
                            memory.Read32(static_cast<Common::ProcessAddress>(
                                static_cast<u64>(context.r[0]) + i * 4));
                        arr += fmt::format(" {:08x}", word);
                    }
                    LOG_INFO(Core_ARM, "Trace thread=98 pollfds:{} ", arr);
                    const u64 tls = context.tpidr;
                    u32 cached{};

                    try {
                        cached = memory.Read32(static_cast<Common::ProcessAddress>(tls + 0x1B0));
                    } catch (...) {}

                    const u32 array_word =
                        memory.Read32(static_cast<Common::ProcessAddress>(
                            static_cast<u64>(context.r[1])));
                    LOG_INFO(Core_ARM,
                             "Trace thread=98 tls={:#x} cached_mask={:#x} array_word={:#x}",
                             tls, cached, array_word);
                }
            }
            // Perform call.
            Svc::Call(system, interface->GetSvcNumber());
            return;
        }

        // Handle external interrupt sources.
        if (interrupt || m_is_single_core) {
            return;
        }
    }
}

void PhysicalCore::LoadContext(const KThread* thread) {
    auto* const process = thread->GetOwnerProcess();
    if (!process) {
        // Kernel threads do not run on emulated CPU cores.
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->SetContext(thread->GetContext());
        interface->SetTpidrroEl0(GetInteger(thread->GetTlsAddress()));
        interface->SetWatchpointArray(&process->GetWatchpoints());
    }
}

void PhysicalCore::LoadSvcArguments(const KProcess& process, std::span<const uint64_t, 8> args) {
    process.GetArmInterface(m_core_index)->SetSvcArguments(args);
}

void PhysicalCore::SaveContext(KThread* thread) const {
    auto* const process = thread->GetOwnerProcess();
    if (!process) {
        // Kernel threads do not run on emulated CPU cores.
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->GetContext(thread->GetContext());
    }
}

void PhysicalCore::SaveSvcArguments(KProcess& process, std::span<uint64_t, 8> args) const {
    process.GetArmInterface(m_core_index)->GetSvcArguments(args);
}

void PhysicalCore::CloneFpuStatus(KThread* dst) const {
    auto* process = dst->GetOwnerProcess();

    Svc::ThreadContext ctx{};
    process->GetArmInterface(m_core_index)->GetContext(ctx);

    dst->GetContext().fpcr = ctx.fpcr;
    dst->GetContext().fpsr = ctx.fpsr;
}

void PhysicalCore::LogBacktrace(KernelCore& kernel) {
    auto* process = GetCurrentProcessPointer(kernel);
    if (!process) {
        return;
    }

    auto* interface = process->GetArmInterface(m_core_index);
    if (interface) {
        interface->LogBacktrace(process);
    }
}

void PhysicalCore::Idle() {
    std::unique_lock lk{m_guard};
    m_on_interrupt.wait(lk, [this] { return m_is_interrupted; });
}

bool PhysicalCore::IsInterrupted() const {
    return m_is_interrupted;
}

void PhysicalCore::Interrupt() {
    // Lock core context.
    std::scoped_lock lk{m_guard};

    // Load members.
    auto* arm_interface = m_arm_interface;
    auto* thread = m_current_thread;

    // Add interrupt flag.
    m_is_interrupted = true;

    // Interrupt ourselves.
    m_on_interrupt.notify_one();

    // If there is no thread running, we are done.
    if (arm_interface == nullptr) {
        return;
    }

    // Interrupt the CPU.
    arm_interface->SignalInterrupt(thread);
}

void PhysicalCore::ClearInterrupt() {
    std::scoped_lock lk{m_guard};
    m_is_interrupted = false;
}

} // namespace Kernel
