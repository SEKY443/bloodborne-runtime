// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: observers in a game's own code (bbport_guest_code_hooks.h). The interface is bbport
// 0.6's (8185b88, deadinside28/bloodborne_pc), itself shadPS4's InstallGuestCodeHook
// (core/cpu_patches.cpp of the shadp2p/BBSC forks, GPL-2.0+), so its hook modules build unchanged.
//
// The trampoline is ours: the site jumps to a stub `int3; <the replaced instructions>; jmp back`.
// The int3 reaches HandleTrap (bbcpu's trap handlers on macOS, SIGTRAP where the game's code runs
// natively), which fills the snapshot from the guest's state, calls the hook and writes the
// registers back; the replaced instructions then run from the stub. bbcpu runs the stubs like
// the game's code (their area is a guest code range), and the recompiled library leaves the
// hooked functions to the translator (their bytes changed).

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include "bbport_guest_code_hooks.h"

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <Zydis/Zydis.h>

#include "cpu/bbcpu.h"
#include "platform.h"

namespace Core {
namespace {

constexpr std::size_t NearJumpBytes = 5;
constexpr std::size_t AreaSize = 256u << 10; // stubs of ~30 bytes each
constexpr std::int64_t JumpReach = 0x7000'0000; // inside a rel32 jump's 2 GiB
constexpr std::size_t MaxHooks = 512;

struct Hook {
    std::uintptr_t trap = 0; ///< the stub's int3
    std::uint64_t tag = 0;
    GuestCodeHook fn = nullptr;
    const std::uint8_t* skip_flag = nullptr;
    std::uintptr_t site = 0;
    std::size_t length = 0;
};
std::mutex hook_mutex;
std::array<Hook, MaxHooks> hooks;
std::atomic<std::size_t> hook_count{0};

std::uint8_t* area = nullptr;
std::size_t area_used = 0;

/// The stubs' memory, within a rel32 jump of the game's code.
std::uint8_t* MapArea(std::uintptr_t code) {
#if defined(__APPLE__)
    // The addresses around the image are the guest's (platform.c): the runtime's low memory,
    // handed out after the image. Data first: arm64 refuses writable code outside MAP_JIT, and
    // bbcpu only reads the stubs.
    auto* p = static_cast<std::uint8_t*>(bb_low_map(AreaSize, PROT_READ | PROT_WRITE));
    const std::int64_t distance = std::int64_t(reinterpret_cast<std::uintptr_t>(p)) - std::int64_t(code);
    if (!p || distance <= -JumpReach || distance >= JumpReach) {
        return nullptr;
    }
    return p;
#else
    const long page = sysconf(_SC_PAGESIZE);
    for (std::int64_t step = 1; step * (64ll << 20) < JumpReach; ++step) {
        for (const std::int64_t sign : {-1ll, 1ll}) {
            const std::int64_t at = std::int64_t(code) + sign * step * (64ll << 20);
            if (at <= 0) {
                continue;
            }
            void* p = mmap(reinterpret_cast<void*>(at & ~std::int64_t(page - 1)), AreaSize,
                           PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) {
                return static_cast<std::uint8_t*>(p);
            }
        }
    }
    return nullptr;
#endif
}

bool WholeAndAbsolute(const std::uint8_t* code, std::size_t size) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    std::size_t offset = 0;
    while (offset < size) {
        ZydisDecodedInstruction instruction{};
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
        const auto status = ZydisDecoderDecodeFull(&decoder, code + offset, size - offset,
                                                   &instruction, operands);
        if (!ZYAN_SUCCESS(status) || instruction.length == 0 || instruction.length > size - offset) {
            std::fprintf(stderr, "Guest code hook at %p: not whole instructions\n",
                         static_cast<const void*>(code));
            return false;
        }
        for (std::uint8_t i = 0; i < instruction.operand_count; ++i) {
            const auto& operand = operands[i];
            if ((operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) ||
                (operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                 (operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP))) {
                std::fprintf(stderr, "Guest code hook at %p: relative addressing\n",
                             static_cast<const void*>(code));
                return false;
            }
        }
        offset += instruction.length;
    }
    return true;
}

std::atomic<bool> code_writable{false};

/// Writes `bytes` over the game's code under a short write permission.
void WriteCode(std::uint8_t* code, const std::uint8_t* bytes, std::size_t size) {
    const long page = sysconf(_SC_PAGESIZE);
    auto* first = reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(code) &
                                                  ~std::uintptr_t(page - 1));
    const std::size_t span = (code + size) - first;
#if defined(__APPLE__)
    mprotect(first, span, PROT_READ | PROT_WRITE);
#else
    mprotect(first, span, PROT_READ | PROT_WRITE | PROT_EXEC);
#endif
    // The jump's last four bytes first, its opcode last: a thread running here meanwhile meets
    // either the old instruction or the whole jump.
    std::memcpy(code + 1, bytes + 1, size - 1);
    __atomic_store_n(code, bytes[0], __ATOMIC_SEQ_CST);
    mprotect(first, span, GuestCodeProtection());
    if (bbcpu_enabled()) {
        bbcpu_invalidate(reinterpret_cast<std::uintptr_t>(code), size);
    }
}

const Hook* HookAt(std::uintptr_t trap) {
    const std::size_t count = hook_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        if (hooks[i].trap == trap) return &hooks[i];
    }
    return nullptr;
}

/// The hook's call; `rip` past the int3. Returns where the guest goes on (the replaced
/// instructions, or the caller with a set skip flag).
void RunHook(const Hook& h, GuestRegisterSnapshot& s, std::uint64_t& rip) {
    h.fn(h.tag, &s);
    if (h.skip_flag && *h.skip_flag) {
        // Function entries only: [rsp] is the return address.
        __atomic_store_n(const_cast<std::uint8_t*>(h.skip_flag), std::uint8_t(0), __ATOMIC_RELAXED);
        rip = *reinterpret_cast<const std::uint64_t*>(s.rsp);
        s.rsp += 8;
    }
}

/// bbcpu: r[] in encoding order (rax rcx rdx rbx rsp rbp rsi rdi r8-r15).
int BbcpuTrap(BbGuestRegs* r) {
    const Hook* h = HookAt(r->rip - 1);
    if (!h) {
        return 0;
    }
    BbTrapState state{};
    const bool full = bbcpu_trap_state(&state) != 0;
    GuestRegisterSnapshot s{};
    s.rax = r->r[0], s.rcx = r->r[1], s.rdx = r->r[2], s.rbx = r->r[3];
    s.rsp = r->r[4], s.rbp = r->r[5], s.rsi = r->r[6], s.rdi = r->r[7];
    s.r8 = r->r[8], s.r9 = r->r[9], s.r10 = r->r[10], s.r11 = r->r[11];
    s.r12 = r->r[12], s.r13 = r->r[13], s.r14 = r->r[14], s.r15 = r->r[15];
    if (full) {
        s.rflags = *state.flags;
        s.mxcsr = *state.mxcsr;
        for (int i = 0; i < 16; ++i) std::memcpy(s.ymm[i].data(), state.ymm[i], 32);
    }
    std::uint64_t rip = r->rip;
    RunHook(*h, s, rip);
    r->r[0] = s.rax, r->r[1] = s.rcx, r->r[2] = s.rdx, r->r[3] = s.rbx;
    r->r[4] = s.rsp, r->r[5] = s.rbp, r->r[6] = s.rsi, r->r[7] = s.rdi;
    r->r[8] = s.r8, r->r[9] = s.r9, r->r[10] = s.r10, r->r[11] = s.r11;
    r->r[12] = s.r12, r->r[13] = s.r13, r->r[14] = s.r14, r->r[15] = s.r15;
    r->rip = rip;
    if (full) {
        *state.flags = s.rflags;
        *state.mxcsr = s.mxcsr;
        for (int i = 0; i < 16; ++i) std::memcpy(state.ymm[i], s.ymm[i].data(), 32);
    }
    return 1;
}

#if defined(__linux__) && defined(__x86_64__)
/// The game's code runs natively: the int3 is a SIGTRAP. XMM registers from the signal frame
/// (the upper halves of YMM are not in it and stay as they are).
struct sigaction previous_trap {};
void OnTrap(int sig, siginfo_t* info, void* context) {
    auto* uc = static_cast<ucontext_t*>(context);
    auto& g = uc->uc_mcontext.gregs;
    const Hook* h = HookAt(std::uintptr_t(g[REG_RIP]) - 1);
    if (!h) {
        if (previous_trap.sa_flags & SA_SIGINFO) {
            if (previous_trap.sa_sigaction) previous_trap.sa_sigaction(sig, info, context);
            return;
        }
        if (previous_trap.sa_handler != SIG_DFL && previous_trap.sa_handler != SIG_IGN &&
            previous_trap.sa_handler) {
            previous_trap.sa_handler(sig);
            return;
        }
        signal(SIGTRAP, SIG_DFL);
        raise(SIGTRAP);
        return;
    }
    GuestRegisterSnapshot s{};
    s.rax = g[REG_RAX], s.rbx = g[REG_RBX], s.rcx = g[REG_RCX], s.rdx = g[REG_RDX];
    s.rsi = g[REG_RSI], s.rdi = g[REG_RDI], s.rbp = g[REG_RBP], s.rsp = g[REG_RSP];
    s.r8 = g[REG_R8], s.r9 = g[REG_R9], s.r10 = g[REG_R10], s.r11 = g[REG_R11];
    s.r12 = g[REG_R12], s.r13 = g[REG_R13], s.r14 = g[REG_R14], s.r15 = g[REG_R15];
    s.rflags = g[REG_EFL];
    auto* fp = uc->uc_mcontext.fpregs;
    if (fp) {
        s.mxcsr = fp->mxcsr;
        for (int i = 0; i < 16; ++i) std::memcpy(s.ymm[i].data(), &fp->_xmm[i], 16);
    }
    std::uint64_t rip = g[REG_RIP];
    RunHook(*h, s, rip);
    g[REG_RAX] = s.rax, g[REG_RBX] = s.rbx, g[REG_RCX] = s.rcx, g[REG_RDX] = s.rdx;
    g[REG_RSI] = s.rsi, g[REG_RDI] = s.rdi, g[REG_RBP] = s.rbp, g[REG_RSP] = s.rsp;
    g[REG_R8] = s.r8, g[REG_R9] = s.r9, g[REG_R10] = s.r10, g[REG_R11] = s.r11;
    g[REG_R12] = s.r12, g[REG_R13] = s.r13, g[REG_R14] = s.r14, g[REG_R15] = s.r15;
    g[REG_EFL] = s.rflags;
    g[REG_RIP] = rip;
    if (fp) {
        fp->mxcsr = s.mxcsr;
        for (int i = 0; i < 16; ++i) std::memcpy(&fp->_xmm[i], s.ymm[i].data(), 16);
    }
}
#endif

/// The trap handler, once.
void InstallTrapHandler() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (bbcpu_enabled()) {
            bbcpu_set_trap_handler(BbcpuTrap);
            return;
        }
#if defined(__linux__) && defined(__x86_64__)
        struct sigaction action {};
        action.sa_sigaction = OnTrap;
        action.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&action.sa_mask);
        sigaction(SIGTRAP, &action, &previous_trap);
#endif
    });
}

} // namespace

int GuestCodeProtection() {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // iOS (proof of concept): bbcpu only reads the game's code; execute would be refused.
    return code_writable.load(std::memory_order_acquire) ? PROT_READ | PROT_WRITE : PROT_READ;
#elif defined(__APPLE__)
    // bbcpu reads the game's code; it never runs natively (no writable and executable pages).
    return code_writable.load(std::memory_order_acquire) ? PROT_READ | PROT_WRITE : PROT_READ | PROT_EXEC;
#else
    return code_writable.load(std::memory_order_acquire) ? PROT_READ | PROT_WRITE | PROT_EXEC
                                                         : PROT_READ | PROT_EXEC;
#endif
}

void KeepGuestCodeWritable() {
    code_writable.store(true, std::memory_order_release);
}

namespace {
bool Install(void* address, std::span<const std::uint8_t> expected, std::uint64_t tag, GuestCodeHook hook,
             const std::uint8_t* skip_flag, bool after) {
    auto* code = static_cast<std::uint8_t*>(address);
    if (!hook || expected.size() < NearJumpBytes) {
        return false;
    }
    std::scoped_lock lock{hook_mutex};
    if (std::memcmp(code, expected.data(), expected.size()) != 0) {
        std::fprintf(stderr, "Guest code hook at %p: the code differs (another build?)\n", address);
        return false;
    }
    if (!WholeAndAbsolute(code, expected.size())) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(code);
    const std::size_t count = hook_count.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
        if (hooks[i].site < begin + expected.size() && begin < hooks[i].site + hooks[i].length) {
            std::fprintf(stderr, "Guest code hook at %p overlaps another one\n", address);
            return false;
        }
    }
    // int3, the replaced instructions, jmp [rip + 0] back (after: the instructions, then int3).
    const std::size_t stub_size = 1 + expected.size() + 14;
    if (count == MaxHooks) {
        std::fprintf(stderr, "Guest code hook at %p: too many hooks\n", address);
        return false;
    }
    if (!area) {
        area = MapArea(begin);
        if (!area) {
            std::fprintf(stderr, "Guest code hook at %p: no trampoline memory in reach\n", address);
            return false;
        }
        if (bbcpu_enabled()) {
            bbcpu_add_guest_code(reinterpret_cast<std::uintptr_t>(area), AreaSize);
        }
    }
    std::uint8_t* stub = area + area_used;
    const std::int64_t rel = std::int64_t(reinterpret_cast<std::uintptr_t>(stub)) -
                             std::int64_t(begin + NearJumpBytes);
    if (area_used + stub_size > AreaSize || rel != std::int32_t(rel)) {
        std::fprintf(stderr, "Guest code hook at %p: no trampoline memory in reach\n", address);
        return false;
    }
#if defined(__APPLE__)
    mprotect(area, AreaSize, PROT_READ | PROT_WRITE);
#endif
    const std::size_t trap_at = after ? expected.size() : 0;
    std::memcpy(stub + (after ? 0 : 1), expected.data(), expected.size());
    stub[trap_at] = 0xcc;
    std::uint8_t* jump = stub + 1 + expected.size();
    const std::uint8_t jmp_rip[] = {0xff, 0x25, 0, 0, 0, 0};
    std::memcpy(jump, jmp_rip, sizeof(jmp_rip));
    const std::uint64_t back = begin + expected.size();
    std::memcpy(jump + sizeof(jmp_rip), &back, sizeof(back));
#if defined(__APPLE__)
    mprotect(area, AreaSize, PROT_READ);
#endif
    area_used += (stub_size + 15) & ~std::size_t(15);

    hooks[count] = Hook{reinterpret_cast<std::uintptr_t>(stub + trap_at), tag, hook, skip_flag, begin,
                        expected.size()};
    hook_count.store(count + 1, std::memory_order_release);
    InstallTrapHandler();

    // The site: a jump to the stub, the rest of the span filled with nops.
    std::vector<std::uint8_t> patch(expected.size(), 0x90);
    patch[0] = 0xe9;
    const std::int32_t rel32 = std::int32_t(rel);
    std::memcpy(patch.data() + 1, &rel32, 4);
    WriteCode(code, patch.data(), patch.size());
    return true;
}

} // namespace

bool InstallGuestCodeHook(void* address, std::span<const std::uint8_t> expected, std::uint64_t tag,
                          GuestCodeHook hook, const std::uint8_t* skip_flag) {
    return Install(address, expected, tag, hook, skip_flag, false);
}

bool InstallGuestCodeHookAfter(void* address, std::span<const std::uint8_t> expected, std::uint64_t tag,
                               GuestCodeHook hook) {
    return Install(address, expected, tag, hook, nullptr, true);
}

} // namespace Core
