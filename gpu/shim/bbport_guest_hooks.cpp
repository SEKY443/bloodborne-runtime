// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_guest_hooks.h"
#include "bbport_guest_call.h"
#include "bbport_guest_code_hooks.h"
#include "bbport_heap_sites.h"
#include "bbport_toggles.h"

#include <array>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <sys/uio.h>
#include "platform.h"
#include "cpu/bbcpu.h"
#include <unistd.h>

extern "C" void runtime_memory_note_write(uintptr_t address, uint64_t size);

namespace BbGuestHooks {
namespace {
using u64 = std::uint64_t;
#define ImageBase u64(BB_IMAGE_BASE)
using u8 = std::uint8_t;

/// The return of the game's GPU memory range allocator 0x26aa070 (rdi allocator, rsi size,
/// rdx alignment; it returns a node: [node + 8] the start, [[node + 0x18] + 8] the end, the next
/// node's start): `add rsp, 0x18` in its epilogue, reached by its own branches only.
constexpr u64 AllocReturn = 0x26aa255;
constexpr unsigned char AllocReturnCode[4] = {0x48, 0x83, 0xc4, 0x18};
RangeCallback on_range_allocated = nullptr;

/// The game's other GPU memory heap (TLSF-like, headers in the memory: free at 0x20858a0, alloc at
/// 0x2085a60), reached through a pointer at 0x54b9f38 set at startup. Its only three callers
/// (wrappers that record {address, size} in a map) pass r8 = rbp - 0x50 for the size handed out and
/// follow the call with `mov rbx, rax` (rax: the address).
constexpr std::array<u64, 3> HeapAllocReturns = {0x11e6629, 0x11e6736, 0x11e6846};
constexpr unsigned char HeapAllocReturnCode[3] = {0x48, 0x89, 0xc3};

/// BB_FRAME_STATS: the range allocator's collector (0x26aa860, `push rbp`; rdi the allocator): its
/// live range count [+0xc8] and byte count [+0xd0] are noted for the frame stats.
constexpr u64 CollectorEntry = 0x26aa860;
constexpr unsigned char CollectorEntryCode[1] = {0x55};

/// BB_HEAP_SITES=1: the game's deferred object release (0xbb5f10) skips the destruction when the
/// registry check (0xbf0790) fails: `js 0xbb5fc7` there, its result counted (eax).
constexpr u64 ReleaseCheck = 0xbb5fb0;
constexpr unsigned char ReleaseCheckCode[2] = {0x78, 0x15};

/// `call memcpy` (0x261a1b0: rdi destination, rsi source, rdx size) in the resource loaders:
/// 0x216d420 and 0x2171d60 copy texture and buffer data into GPU memory (most of the write
/// faults while areas stream in).
struct CallSite {
    u64 offset;
    unsigned char code[5];
};
constexpr std::array<CallSite, 2> MemcpySites = {{
    {0x216d5ea, {0xe8, 0xc1, 0xcb, 0x4a, 0x00}},
    {0x2171fea, {0xe8, 0xc1, 0x81, 0x4a, 0x00}},
}};
std::array<std::atomic<u64>, MemcpySites.size()> hits{};

/// The game's memcpy at those sites: the GPU side is told before (its write traps lift: no fault
/// per page) and after the copy (the data now there), as for file reads.
void* LoaderCopy(void* dst, const void* src, std::size_t size) {
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    std::memcpy(dst, src, size);
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    return dst;
}

/// bbport: native detours (default; BB_GUEST_HOOK_TRAPS=1: int3 traps). Each hooked site jumps to
/// a stub next to the image that calls the handler and runs the instructions the jump covers;
/// the copy call sites call LoaderCopy through a trampoline there. An int3 trap costs a signal
/// (a few microseconds in the kernel) per call; a detour costs a call. The int3 written first
/// stays the fallback while the jump's bytes go in: OnTrap sends a thread that meets it to the stub.
struct NativeSite {
    u64 offset;
    std::atomic<u64> stub{0};
};
std::array<NativeSite, 4> native_sites{{{AllocReturn}, {HeapAllocReturns[0]}, {HeapAllocReturns[1]},
                                        {HeapAllocReturns[2]}}};
u8* stub_page = nullptr;
std::size_t stub_used = 0;
constexpr std::size_t StubPageSize = 4096;

/// A page within +-2 GiB of the image (below its base: the image grows upwards). None on macOS:
/// the game's code runs in bbcpu there, which cannot enter host code from guest code (the hooks
/// stay int3 traps, handled through bbcpu_set_trap_handler).
u8* MapStubPage() {
#if defined(__APPLE__)
    return nullptr;
#else
    for (u64 k = 1; k <= 64; ++k) {
        const u64 hint = ImageBase - k * (16ull << 20);
        void* p = mmap(reinterpret_cast<void*>(hint), StubPageSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) {
            return static_cast<u8*>(p);
        }
    }
    return nullptr;
#endif
}

struct Emitter {
    std::array<u8, 256> code{};
    std::size_t size = 0;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            code[size++] = b;
        }
    }
    void U32(std::uint32_t v) {
        std::memcpy(&code[size], &v, 4);
        size += 4;
    }
    void U64(u64 v) {
        std::memcpy(&code[size], &v, 8);
        size += 8;
    }
    /// Saves the caller-saved registers (the handler may change them), aligns the stack.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12 (holds the unaligned stack pointer)
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void CallAbs(const void* fn) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(fn));
        Bytes({0xff, 0xd0}); // call rax
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }};

/// Copies `e` into the stub page; returns its address there, or 0.
u64 PlaceStub(const Emitter& e) {
    if (!stub_page || stub_used + e.size > StubPageSize) {
        return 0;
    }
    u8* at = stub_page + stub_used;
    std::memcpy(at, e.code.data(), e.size);
    stub_used += (e.size + 15) & ~std::size_t(15);
    return reinterpret_cast<u64>(at);
}

bool Rel32To(u64 from_next, u64 to, std::uint32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = std::uint32_t(std::int32_t(d));
    return true;
}

/// Replaces bytes [1, 5) at `address` (byte 0 is the trap already there), then byte 0.
void WriteJumpOverTrap(u64 address, u8 opcode, std::uint32_t rel) {
    const long page_size = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    const std::size_t span = (address + 5) - reinterpret_cast<u64>(page);
    mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
    auto* p = reinterpret_cast<u8*>(address);
    for (int i = 0; i < 4; ++i) {
        __atomic_store_n(p + 1 + i, u8(rel >> (8 * i)), __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(p, opcode, __ATOMIC_SEQ_CST);
    mprotect(page, span, PROT_READ | PROT_EXEC);
}

void AllocHook(u64 node) {
    if (!node) {
        return;
    }
    const u64 start = *reinterpret_cast<const u64*>(node + 8);
    const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
    const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
    if (end > start && end - start < (u64(1) << 32)) {
        on_range_allocated(start, end - start);
    }
}

void HeapHook(u64 address, u64 size) {
    if (address != 0 && size != 0 && size < (u64(1) << 32)) {
        on_range_allocated(address, size);
    }
}

/// The detour of a site whose int3 is in place (Patch). False: left as a trap.
bool InstallDetour(NativeSite& site) {
    const u64 address = ImageBase + site.offset;
    Emitter e;
    u64 back = 0;
    if (site.offset == AllocReturn) {
        // `add rsp, 0x18; pop rbx` (5 bytes) in the allocator's epilogue; rax: the node.
        if (reinterpret_cast<const u8*>(address)[4] != 0x5b) {
            return false;
        }
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xc7}); // mov rdi, rax (unchanged by the saving)
        e.CallAbs(reinterpret_cast<const void*>(&AllocHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x83, 0xc4, 0x18}); // add rsp, 0x18
        e.Bytes({0x5b});                   // pop rbx
        back = address + 5;
    } else {
        // `mov rbx, rax; test rbx, rbx` (6 bytes) after the heap call; [rbp - 0x50]: the size.
        const auto* code = reinterpret_cast<const u8*>(address);
        if (code[3] != 0x48 || code[4] != 0x85 || code[5] != 0xdb) {
            return false;
        }
        e.Bytes({0x48, 0x89, 0xc3});                               // mov rbx, rax
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); // lea rsp, [rsp - 128]
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xdf});       // mov rdi, rbx
        e.Bytes({0x48, 0x8b, 0x75, 0xb0}); // mov rsi, [rbp - 0x50]
        e.CallAbs(reinterpret_cast<const void*>(&HeapHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
        e.Bytes({0x48, 0x85, 0xdb});                               // test rbx, rbx
        back = address + 6;
    }
    // jmp back
    const u64 jmp_at = reinterpret_cast<u64>(stub_page + stub_used) + e.size;
    std::uint32_t rel_back = 0;
    if (!Rel32To(jmp_at + 5, back, rel_back)) {
        return false;
    }
    e.Bytes({0xe9});
    e.U32(rel_back);
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    site.stub.store(stub, std::memory_order_release); // a thread at the trap now goes there
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The copy call sites: the call's target becomes a trampoline to LoaderCopy.
bool InstallCallDetour(u64 offset) {
    Emitter e;
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(reinterpret_cast<u64>(&LoaderCopy));
    const u64 stub = PlaceStub(e);
    const u64 address = ImageBase + offset;
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe8, rel);
    return true;
}
struct sigaction previous_action {};

/// The registers a hook reads or changes (from a signal context or from bbcpu).
struct TrapRegs {
    u64 rax, rbx, rsp, rbp, rdi, rip; // rip: past the int3
};

bool HandleTrap(TrapRegs& g) {
    const u64 rip = g.rip - 1; // past the int3
    for (const auto& site : native_sites) {
        if (rip == ImageBase + site.offset) {
            if (const u64 stub = site.stub.load(std::memory_order_acquire)) {
                g.rip = stub; // the detour is going in: its stub runs the site
                return true;
            }
        }
    }
    if (rip == ImageBase + ReleaseCheck) {
        const auto result = static_cast<std::int32_t>(g.rax & 0xffffffff);
        BbHeapSites::NoteReleaseCheck(std::uint32_t(result));
        g.rip = u64(ImageBase + (result < 0 ? 0xbb5fc7 : ReleaseCheck + 2));
        return true;
    }
    if (rip == ImageBase + CollectorEntry) {
        const u64 allocator = g.rdi;
        for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
            u64 expected = 0;
            if (BbStats::range_allocators[i].load(std::memory_order_relaxed) == allocator ||
                BbStats::range_allocators[i].compare_exchange_strong(expected, allocator)) {
                BbStats::range_live[i].store(*reinterpret_cast<const u64*>(allocator + 0xc8),
                                             std::memory_order_relaxed);
                BbStats::range_bytes[i].store(*reinterpret_cast<const u64*>(allocator + 0xd0),
                                              std::memory_order_relaxed);
                break;
            }
        }
        g.rsp -= 8; // push rbp
        *reinterpret_cast<u64*>(g.rsp) = g.rbp;
        g.rip = u64(ImageBase + CollectorEntry + 1);
        return true;
    }
    for (const u64 site : HeapAllocReturns) {
        if (rip != ImageBase + site) {
            continue;
        }
        const u64 address = g.rax;
        const u64 size = *reinterpret_cast<const u64*>(g.rbp - 0x50);
        if (address != 0 && size != 0 && size < (u64(1) << 32)) {
            on_range_allocated(address, size);
        }
        g.rbx = g.rax;
        g.rip = u64(ImageBase + site + sizeof(HeapAllocReturnCode));
        return true;
    }
    if (rip == ImageBase + AllocReturn) {
        if (const u64 node = g.rax) {
            const u64 start = *reinterpret_cast<const u64*>(node + 8);
            const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
            const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
            if (end > start && end - start < (u64(1) << 32)) {
                on_range_allocated(start, end - start);
            }
        }
        g.rsp += 0x18;
        g.rip = u64(ImageBase + AllocReturn + sizeof(AllocReturnCode));
        return true;
    }
    for (std::size_t i = 0; i < MemcpySites.size(); ++i) {
        const auto& site = MemcpySites[i];
        if (rip != ImageBase + site.offset) {
            continue;
        }
        hits[i].fetch_add(1, std::memory_order_relaxed);
        // The call, to LoaderCopy instead of the game's memcpy: push the return address, jump.
        const u64 ret = ImageBase + site.offset + 5;
        g.rsp -= 8;
        *reinterpret_cast<u64*>(g.rsp) = ret;
        g.rip = u64(reinterpret_cast<u64>(&LoaderCopy));
        return true;
    }
    return false;
}

/// bbcpu (translated guest code): the int3 reaches this instead of SIGTRAP.
int BbcpuTrap(BbGuestRegs* r) {
    TrapRegs g{r->r[0], r->r[3], r->r[4], r->r[5], r->r[7], r->rip};
    if (!HandleTrap(g)) {
        return 0;
    }
    r->r[0] = g.rax; r->r[3] = g.rbx; r->r[4] = g.rsp; r->r[5] = g.rbp; r->r[7] = g.rdi;
    r->rip = g.rip;
    return 1;
}

void OnTrap(int sig, siginfo_t* info, void* context) {
    auto* uc = static_cast<ucontext_t*>(context);
    TrapRegs g{u64(BB_UC_RAX(uc)), u64(BB_UC_RBX(uc)), u64(BB_UC_RSP(uc)), u64(BB_UC_RBP(uc)),
               u64(BB_UC_RDI(uc)), u64(BB_UC_RIP(uc))};
    if (HandleTrap(g)) {
        BB_UC_RAX(uc) = g.rax; BB_UC_RBX(uc) = g.rbx; BB_UC_RSP(uc) = g.rsp;
        BB_UC_RBP(uc) = g.rbp; BB_UC_RDI(uc) = g.rdi; BB_UC_RIP(uc) = g.rip;
        return;
    }
    if (previous_action.sa_flags & SA_SIGINFO) {
        if (previous_action.sa_sigaction) {
            previous_action.sa_sigaction(sig, info, context);
            return;
        }
    } else if (previous_action.sa_handler != SIG_DFL && previous_action.sa_handler != SIG_IGN &&
               previous_action.sa_handler) {
        previous_action.sa_handler(sig);
        return;
    }
    signal(SIGTRAP, SIG_DFL);
    raise(SIGTRAP);
}

bool Patch(u64 address, const unsigned char* expected, std::size_t size) {
    unsigned char bytes[16]{};
    if (bb_read_memory(bytes, address, size) != size ||
        std::memcmp(bytes, expected, size) != 0) {
        return false;
    }
    const long page_size = sysconf(_SC_PAGESIZE);
    void* page = reinterpret_cast<void*>(address & ~u64(page_size - 1));
    const std::size_t span = (address + size) - reinterpret_cast<u64>(page);
    if (mprotect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<unsigned char*>(address), static_cast<unsigned char>(0xcc),
                     __ATOMIC_SEQ_CST);
    mprotect(page, span, PROT_READ | PROT_EXEC);
    if (bbcpu_enabled()) {
        bbcpu_invalidate(address, size);
    }
    return true;
}
// ---- GX occlusion queries without a handle (bbport 0.7: PR #343 by whp199, issue #336) ----
//
// The fault at 0x2169edc on GXRenderThread (app issues #3 and #7, once in hours of play): a query
// (0x30 bytes, made by its pool through 0xe2f080(query, 1)) gets its handle, its first field, from
// 0x21674f0, which returns 8 or 9 and leaves the handle null when the device's allocators fail; the
// pool does not look. The render command then begins the query (0xe2f130: 0x2169ec0 reads the
// handle) and ends it (0xe2f160). The guard: at the begin, a query whose creation failed is created
// again; one still without a handle is neither begun nor ended (both return true at once, as the
// begin does for a query begun already), so its state stays 0 and its result 0 (nothing visible).
// The cause (the device's heap full late in a long session) is bbport's guess. bbport patches x86
// stubs over the two state accesses; here hooks at the two entries do it (Core::InstallGuestCodeHook
// with a skip flag), which also works where the game's code runs in bbcpu. Both functions run on
// GXRenderThread only, so one skip flag each is enough.
// BB_GX_QUERY_GUARD=0: not installed. BB_GX_QUERY_FAIL=N (diagnostics): every N-th creation fails
// as with a full heap, so the guard's paths run in any session (N = 2 reaches a scene).
constexpr u64 GxQueryBegin = 0xe2f130;
constexpr std::array<u8, 8> GxQueryBeginCode = {0x55, 0x48, 0x89, 0xe5,  // push rbp; mov rbp, rsp
                                                0x83, 0x7f, 0x08, 0x00}; // cmp dword [rdi + 8], 0
constexpr u64 GxQueryEnd = 0xe2f160;
constexpr std::array<u8, 11> GxQueryEndCode = {0x55, 0x48, 0x89, 0xe5,                    // push rbp; mov rbp, rsp
                                               0xc7, 0x47, 0x08, 0x01, 0x00, 0x00, 0x00}; // mov dword [rdi + 8], 1
constexpr u64 GxQueryCreate = 0xe2f080;
constexpr u64 GxQueryRelease = 0xe2f020;
/// The creation's common exit: eax the result, r14 the query, rbx null when the handle's
/// allocation failed.
constexpr u64 GxQueryCreateExit = 0x21675b0;
constexpr std::array<u8, 7> GxQueryCreateExitCode = {0x49, 0x8b, 0x0f, 0x48, 0x3b, 0x4d, 0xe0};
std::array<std::atomic<u64>, 256> gx_query_failed{}; ///< queries whose creation failed (0: free)
std::atomic<u64> gx_query_failures{0}, gx_query_recreated{0}, gx_query_skipped{0};
std::atomic<long long> gx_query_failure_told{0}, gx_query_guard_told{0};
std::uint8_t gx_query_skip_begin = 0, gx_query_skip_end = 0;

/// True the first time and then at most once a minute.
bool OncePerMinute(std::atomic<long long>& last) {
    timespec now_ts{};
    clock_gettime(CLOCK_MONOTONIC, &now_ts);
    const long long now = now_ts.tv_sec * 1000LL + now_ts.tv_nsec / 1000000;
    long long seen = last.load(std::memory_order_relaxed);
    return (seen == 0 || now - seen >= 60000) && last.compare_exchange_strong(seen, now);
}

bool GxQueryFailed(u64 query) {
    for (const auto& slot : gx_query_failed) {
        if (slot.load(std::memory_order_relaxed) == query) return true;
    }
    return false;
}

void __attribute__((sysv_abi)) GxQueryCreated(u64, const Core::GuestRegisterSnapshot* r) {
    auto status = std::uint32_t(r->rax);
    const u64 query = r->r14;
    static const long fail_every = [] {
        const char* env = std::getenv("BB_GX_QUERY_FAIL");
        return env ? std::strtol(env, nullptr, 10) : 0l;
    }();
    static std::atomic<u64> creations{0};
    const bool simulated = status == 0 && fail_every > 0 &&
                           (creations.fetch_add(1) + 1) % u64(fail_every) == 0;
    if (simulated) {
        // The game's release takes the new handle back, as after a failed allocation.
        const std::uint64_t args[] = {query};
        runtime_guest_call(reinterpret_cast<const void*>(ImageBase + GxQueryRelease), 1, args);
        const_cast<Core::GuestRegisterSnapshot*>(r)->rax = status = 9;
    }
    if (status == 0) {
        for (auto& slot : gx_query_failed) {
            u64 expected = query;
            if (slot.load(std::memory_order_relaxed) == query) slot.compare_exchange_strong(expected, 0);
        }
        return;
    }
    const bool known = GxQueryFailed(query);
    for (auto& slot : gx_query_failed) {
        u64 expected = 0;
        if (known || slot.compare_exchange_strong(expected, query)) break;
    }
    const u64 n = gx_query_failures.fetch_add(1) + 1;
    if (!OncePerMinute(gx_query_failure_told)) return;
    std::printf("Guest hooks: a GX occlusion query could not be created (error %u: %s), %llu so far; "
                "the query guard creates it again before it begins\n", status,
                simulated     ? "BB_GX_QUERY_FAIL"
                : status == 9 ? (r->rbx ? "no memory for its data" : "no memory for its handle")
                              : "the device or its data not ready",
                static_cast<unsigned long long>(n));
}

/// The begin (rdi the query): a query about to begin without a handle is created again if its
/// creation failed (the game's own 0xe2f080, as the pool does); still without one, the begin
/// returns true at once.
void __attribute__((sysv_abi)) GxQueryBeginHook(u64, const Core::GuestRegisterSnapshot* r) {
    const u64 query = r->rdi;
    if (*reinterpret_cast<const volatile std::uint32_t*>(query + 8) != 0 ||
        *reinterpret_cast<const volatile u64*>(query) != 0) {
        return; // begun already, or with its handle: the game's own path
    }
    const bool failed = GxQueryFailed(query);
    if (failed) {
        const std::uint64_t args[] = {query, 1};
        runtime_guest_call(reinterpret_cast<const void*>(ImageBase + GxQueryCreate), 2, args);
    }
    const bool created = *reinterpret_cast<const volatile u64*>(query) != 0;
    const u64 recreated = gx_query_recreated.fetch_add(created) + created;
    const u64 skipped = gx_query_skipped.fetch_add(!created) + !created;
    if (!created) {
        auto* regs = const_cast<Core::GuestRegisterSnapshot*>(r);
        regs->rax = (regs->rax & ~u64(0xff)) | 1; // mov al, 1: the begin's own result
        gx_query_skip_begin = 1;
    }
    if (!OncePerMinute(gx_query_guard_told)) return;
    std::printf("Guest hooks: GX occlusion query %#llx had no handle at its begin (%s): %llu created "
                "again, %llu not begun (no fault; nothing visible) so far\n",
                static_cast<unsigned long long>(query),
                failed ? "its creation had failed" : "no failed creation seen: released?",
                static_cast<unsigned long long>(recreated), static_cast<unsigned long long>(skipped));
}

/// The end (rdi the query): only a query with a handle is ended.
void __attribute__((sysv_abi)) GxQueryEndHook(u64, const Core::GuestRegisterSnapshot* r) {
    if (*reinterpret_cast<const volatile u64*>(r->rdi) != 0) return;
    auto* regs = const_cast<Core::GuestRegisterSnapshot*>(r);
    regs->rax = (regs->rax & ~u64(0xff)) | 1;
    gx_query_skip_end = 1;
}
} // namespace

// ---- Fixes to the game's own code (bbport 0.7, from the SrShadowy fork's engine fixes) ----
// From deadinside28/bloodborne_pc 0.7 (0fc2921, gpu/shim/bbport_engine_fixes.cpp), which took them
// from SrShadowy's fork (extra/core/engine_fixes); its light-grid setting is not taken.
namespace {
/// vcvttss2si rcx, xmm0: the frame's play time in ms (xmm0 = dt * 1000), added to the save's play
/// time (GameDataMan + 0x94) right after.
constexpr u64 PlayTimeConvert = 0x19419de;
constexpr std::array<u8, 5> PlayTimeConvertCode = {0xc4, 0xe1, 0xfa, 0x2c, 0xc8};
/// Havok's 96-byte hit collector push_back (rdi = the array, rsi = the hit).
constexpr u64 HitPushBack = 0xb84ce0;
constexpr std::array<u8, 6> HitPushBackEntry = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56};
/// The step before a save serializes the player's data.
constexpr u64 CharaStep = 0x1c43ee0;
constexpr std::array<u8, 6> CharaStepEntry = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
constexpr u64 GameDataMan = 0x553b130;
std::atomic<u64> hits_dropped{0}, chara_ids_cleared{0};

// The game truncates each frame's milliseconds: at 60 FPS (16.67 ms) a frame counted 16 ms and the
// play time ran 4 % slow, more the higher the frame rate. The fraction is carried to the next frame
// (one thread: the game's main loop).
void __attribute__((sysv_abi)) PlayTimeHook(u64, const Core::GuestRegisterSnapshot* r) {
    static double carry_ms = 0.0;
    auto* regs = const_cast<Core::GuestRegisterSnapshot*>(r);
    float ms = 0.0f;
    std::memcpy(&ms, regs->ymm[0].data(), sizeof(ms));
    const double total = double(ms) + carry_ms;
    if (!(total > 0.0) || total > 60000.0) {
        carry_ms = 0.0; // a pause or a hitch: as the game counted it
        return;
    }
    const double whole = double(u64(total));
    carry_ms = total - whole;
    const float out = float(whole);
    std::memcpy(regs->ymm[0].data(), &out, sizeof(out));
}

// A degenerate query (a zero-length ray while the world is paused) filled the collector without end
// until an allocation failed. Past BB_HIT_COLLECTOR_CAP (65536) hits the call writes into this
// thread's scratch array instead: the game's function runs as it is, nothing is skipped.
struct alignas(16) ScratchArray {
    u8 head[0x20];
    void* data;
    std::int32_t size;
    std::int32_t capacity;
};
void __attribute__((sysv_abi)) HitPushBackHook(u64, const Core::GuestRegisterSnapshot* r) {
    static const std::int32_t cap = [] {
        const char* env = std::getenv("BB_HIT_COLLECTOR_CAP");
        const long v = env ? std::strtol(env, nullptr, 10) : 65536;
        return std::int32_t(v > 0 ? v : 65536);
    }();
    const auto* array = reinterpret_cast<const u8*>(r->rdi);
    if (!array) {
        return;
    }
    std::int32_t size = 0;
    void* data = nullptr;
    std::memcpy(&size, array + 0x28, sizeof(size));
    std::memcpy(&data, array + 0x20, sizeof(data));
    if (size < cap && (data || size == 0)) {
        return;
    }
    alignas(16) thread_local std::array<u8, 96> hit{};
    thread_local ScratchArray scratch{};
    scratch.data = hit.data();
    scratch.size = 0;
    scratch.capacity = 1;
    const_cast<Core::GuestRegisterSnapshot*>(r)->rdi = reinterpret_cast<u64>(&scratch);
    if (hits_dropped.fetch_add(1, std::memory_order_relaxed) == 0) {
        std::printf("Engine fixes: a hit collector reached %d hits: further hits dropped\n", int(size));
    }
}

// An invalid character id (0x8000000000000000) in the player's data (GameDataMan + 8 -> +0x690),
// cleared before the save serializes it (SrShadowy's fork: corrupted saves).
void __attribute__((sysv_abi)) CharaStepHook(u64, const Core::GuestRegisterSnapshot*) {
    u64 manager = 0, player = 0, id = 0;
    std::memcpy(&manager, reinterpret_cast<const void*>(ImageBase + GameDataMan), sizeof(manager));
    if (manager < 0x100000000ull) {
        return;
    }
    std::memcpy(&player, reinterpret_cast<const void*>(manager + 0x8), sizeof(player));
    if (player < 0x100000000ull) {
        return;
    }
    auto* field = reinterpret_cast<u8*>(player + 0x690);
    std::memcpy(&id, field, sizeof(id));
    if (id == 0x8000000000000000ull) {
        const u64 none = 0;
        std::memcpy(field, &none, sizeof(none));
        if (chara_ids_cleared.fetch_add(1, std::memory_order_relaxed) == 0) {
            std::printf("Engine fixes: an invalid character id cleared before a save\n");
        }
    }
}
} // namespace

void InstallEngineFixes() {
    const auto install = [](const char* env, u64 offset, auto code, auto hook) -> const char* {
        if (const char* v = std::getenv(env); v && v[0] == '0') {
            return "off";
        }
        return Core::InstallGuestCodeHook(reinterpret_cast<void*>(ImageBase + offset), code, offset, hook)
                   ? "on" : "not installed (other code there)";
    };
    const char* play = install("BB_FIX_PLAY_TIME", PlayTimeConvert, PlayTimeConvertCode, PlayTimeHook);
    const char* hits = install("BB_FIX_HIT_COLLECTOR", HitPushBack, HitPushBackEntry, HitPushBackHook);
    const char* chara = install("BB_FIX_CHARA_ID", CharaStep, CharaStepEntry, CharaStepHook);
    std::printf("Engine fixes (bbport 0.7): high frame rate play time %s, hit collector cap %s, save "
                "character id guard %s\n", play, hits, chara);
}

bool InstallGxQueryGuard() {
    if (const char* env = std::getenv("BB_GX_QUERY_GUARD"); env && env[0] == '0') {
        return false;
    }
    if (!Core::InstallGuestCodeHook(reinterpret_cast<void*>(ImageBase + GxQueryBegin), GxQueryBeginCode, 0,
                                    GxQueryBeginHook, &gx_query_skip_begin) ||
        !Core::InstallGuestCodeHook(reinterpret_cast<void*>(ImageBase + GxQueryEnd), GxQueryEndCode, 0,
                                    GxQueryEndHook, &gx_query_skip_end)) {
        return false;
    }
    // Without the creation observer a query without a handle is only skipped.
    Core::InstallGuestCodeHook(reinterpret_cast<void*>(ImageBase + GxQueryCreateExit),
                               GxQueryCreateExitCode, 0, GxQueryCreated);
    return true;
}

void Install(RangeCallback on_gpu_range_allocated) {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) {
        return;
    }
    struct sigaction action {};
    action.sa_sigaction = OnTrap;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTRAP, &action, &previous_action);
    if (bbcpu_enabled()) {
        bbcpu_set_trap_handler(BbcpuTrap);
    }
    int patched = 0;
    for (const auto& site : MemcpySites) {
        patched += Patch(ImageBase + site.offset, site.code, sizeof(site.code));
    }
    std::printf("Guest hooks: %d of %zu resource copy sites tell the GPU side of their writes\n",
                patched, MemcpySites.size());
    on_range_allocated = on_gpu_range_allocated;
    const bool allocator = Patch(ImageBase + AllocReturn, AllocReturnCode, sizeof(AllocReturnCode));
    // BB_HEAP_HOOKS=0 (bisecting): the heap's ranges are not reported.
    const char* heap_env = std::getenv("BB_HEAP_HOOKS");
    const bool heap_hooks = !heap_env || heap_env[0] != '0';
    if (BbStats::enabled) {
        Patch(ImageBase + CollectorEntry, CollectorEntryCode, sizeof(CollectorEntryCode));
    }
    int heap_sites = 0;
    for (const u64 site : HeapAllocReturns) {
        if (!heap_hooks) {
            break;
        }
        heap_sites += Patch(ImageBase + site, HeapAllocReturnCode, sizeof(HeapAllocReturnCode));
    }
    std::printf("Guest hooks: GPU heap allocations seen at %d of %zu call sites\n", heap_sites,
                HeapAllocReturns.size());
    std::printf("Guest hooks: GPU memory allocator %s\n", allocator ? "tells the GPU side of new ranges" : "not found");
    // Native detours over the traps just placed (BB_GUEST_HOOK_TRAPS=1 keeps the traps).
    const char* traps_env = std::getenv("BB_GUEST_HOOK_TRAPS");
    if (!(traps_env && traps_env[0] == '1') && (stub_page = MapStubPage()) != nullptr) {
        int detours = 0;
        for (const auto& site : MemcpySites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallCallDetour(site.offset);
            }
        }
        for (auto& site : native_sites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.offset)[0] == 0xcc) {
                detours += InstallDetour(site);
            }
        }
        mprotect(stub_page, StubPageSize, PROT_READ | PROT_EXEC);
        std::printf("Guest hooks: %d native detours (no traps; BB_GUEST_HOOK_TRAPS=1: int3)\n",
                    detours);
    }
    BbHeapSites::Install();
    if (const char* env = std::getenv("BB_HEAP_SITES"); env && env[0] == '1') {
        Patch(ImageBase + ReleaseCheck, ReleaseCheckCode, sizeof(ReleaseCheckCode));
    }
}
} // namespace BbGuestHooks
