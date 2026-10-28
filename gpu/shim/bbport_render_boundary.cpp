// SPDX-License-Identifier: GPL-2.0-or-later
// bloodborne-runtime: the engine's render boundary, observing (bbport_render_boundary.h).
//
// A draw method's entry hook reads the draw state it is given (layouts: bloodborne-recomp's
// annotations/structs.h) into a pending record for its thread. libGnm's draw functions are hooked
// too: at their entry the pending record is filed under a number, and a PM4 NOP carrying that
// number is written at the command buffer's write pointer, just before the draw's packet. The
// engine copies its recorded commands into the buffers it submits, so the tag travels with the
// draw; the GPU ignores it. The packet reader runs later on the GPU thread: a tagged NOP followed
// at once by a draw packet names that draw's record.
#include "bbport_render_boundary.h"
#include "bbport_guest_code_hooks.h"

#include "video_core/amdgpu/regs.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

namespace BbRenderBoundary {
namespace {
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;

enum Method : u32 { Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, DrawIndirect, DrawIndexedIndirect, Methods };
constexpr const char* method_names[Methods] = {"draw", "drawIndexed", "drawInstanced", "drawIndexedInstanced",
                                               "drawIndirect", "drawIndexedIndirect"};
// CG::GraphicsContext's draw methods and libGnm's draw functions (annotations/functions.tsv, 1.09).
constexpr u64 method_offsets[Methods] = {0x21696c0, 0x2169780, 0x2169840, 0x2169900, 0x21699d0, 0x2169ab0};
constexpr u64 gnm_offsets[] = {
    0x1074360, // Gnm::DrawCommandBuffer::drawIndex
    0x10741b0, // drawIndexAuto
    0x1074420, // drawIndexOffset
    0x10744d0, // drawIndirect
    0x10746f0, // drawIndexIndirect
};
constexpr u64 GnmTag = 0x100;

// CgDrawState (annotations/structs.h).
constexpr u64 Topology = 0x8, IndexBuffer = 0x10, IndexFormat = 0x18, IndexOffset = 0x1c, Stages = 0x598,
              StageSize = 0x4f8, StageShader = 0x4f0, ShaderCode = 0x108, BufferAddress = 0x20, BufferManaged = 0x40;
enum Stage { Vertex, Hull, Domain, Geometry, Pixel, StageCount };
constexpr const char* stage_names[StageCount] = {"vertex", "hull", "domain", "geometry", "pixel"};
// libGnm's command buffer: the end of its space and the write pointer.
constexpr u64 CommandEnd = 0x8, CommandPointer = 0x10;
// The tag: a type-3 NOP with two payload dwords, the magic and the record's number.
constexpr u32 NopTwoDwords = 0xc0011000;

struct Record {
    u64 key; // the record's number
    u32 method, topology, index_format, count, instances;
    u64 index_start; // the first index's address; 0 when unknown (a managed buffer)
    u64 shader[StageCount];
};

struct Pending {
    bool valid;
    Record record;
};
thread_local Pending pending;

// Records by number, in a ring; a sequence word per slot (odd while written).
constexpr std::size_t Slots = 1 << 16;
struct Slot {
    std::atomic<u32> sequence;
    Record record;
};
Slot* slots = nullptr;
bool observing = false;
u64 image_base = 0;
std::atomic<u64> method_calls[Methods], filed{0}, gnm_without_method{0}, no_room{0}, next_number{1};

std::size_t SlotOf(u64 key) {
    return std::size_t(key) & (Slots - 1);
}

void File(const Record& record) {
    Slot& slot = slots[SlotOf(record.key)];
    u32 s = slot.sequence.load(std::memory_order_relaxed);
    if (s & 1 || !slot.sequence.compare_exchange_strong(s, s + 1, std::memory_order_acquire)) {
        return; // another thread is writing it: drop this one
    }
    std::memcpy(&slot.record, &record, sizeof(record));
    slot.sequence.store(s + 2, std::memory_order_release);
}

bool Find(u64 key, Record& out) {
    const Slot& slot = slots[SlotOf(key)];
    for (int attempt = 0; attempt < 4; ++attempt) {
        const u32 s = slot.sequence.load(std::memory_order_acquire);
        if (s & 1) {
            continue;
        }
        std::memcpy(&out, &slot.record, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.sequence.load(std::memory_order_relaxed) == s) {
            return out.key == key;
        }
    }
    return false;
}

template <typename T>
T Read(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return value;
}

void __attribute__((sysv_abi)) OnMethod(u64 tag, const Core::GuestRegisterSnapshot* r) {
    method_calls[tag].fetch_add(1, std::memory_order_relaxed);
    Record& d = pending.record;
    d = {};
    d.method = u32(tag);
    const u64 state = r->rsi;
    const u32 a = u32(r->rdx), b = u32(r->rcx), c = u32(r->r8);
    u32 first_index = 0;
    switch (tag) {
    case Draw: d.count = a, d.instances = 1; break;
    case DrawIndexed: d.count = a, first_index = b, d.instances = 1; break;
    case DrawInstanced: d.count = a, d.instances = b; break;
    case DrawIndexedInstanced: d.count = a, d.instances = b, first_index = c; break;
    default: break; // indirect: the counts are in memory
    }
    d.topology = Read<u32>(state + Topology);
    d.index_format = Read<u32>(state + IndexFormat);
    if (tag == DrawIndexed || tag == DrawIndexedInstanced) {
        const u64 buffer = Read<u64>(state + IndexBuffer);
        if (buffer && !Read<u8>(buffer + BufferManaged)) {
            const u64 size = d.index_format == 0x2a ? 4 : 2;
            d.index_start = Read<u64>(buffer + BufferAddress) + Read<u32>(state + IndexOffset) + first_index * size;
        }
    }
    for (int s = 0; s < StageCount; ++s) {
        const u64 shader = Read<u64>(state + Stages + s * StageSize + StageShader);
        d.shader[s] = shader ? Read<u64>(shader + ShaderCode) : 0;
    }
    pending.valid = true;
}

void __attribute__((sysv_abi)) OnGnmDraw(u64, const Core::GuestRegisterSnapshot* r) {
    if (!pending.valid) {
        gnm_without_method.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    pending.valid = false;
    const u64 buffer = r->rdi;
    u32* at = reinterpret_cast<u32*>(Read<u64>(buffer + CommandPointer));
    const u64 end = Read<u64>(buffer + CommandEnd);
    if (end < reinterpret_cast<u64>(at + 3)) {
        no_room.fetch_add(1, std::memory_order_relaxed); // the draw refills the buffer itself
        return;
    }
    const u64 number = next_number.fetch_add(1, std::memory_order_relaxed);
    pending.record.key = number;
    File(pending.record);
    at[0] = NopTwoDwords;
    at[1] = TagPayload;
    at[2] = u32(number);
    const u64 next = reinterpret_cast<u64>(at + 3);
    std::memcpy(reinterpret_cast<void*>(buffer + CommandPointer), &next, sizeof(next));
    filed.fetch_add(1, std::memory_order_relaxed);
}

// The packet reader's side: GPU thread only.
enum Field { Found, Missing, TopologyField, IndexTypeField, CountField, InstancesField, IndexStartField, Fields };
constexpr const char* field_names[Fields] = {"found", "missing", "topology", "index type", "count", "instances",
                                             "index start"};
struct Tally {
    u64 compared[Fields], differ[Fields];
    u64 shader_compared[StageCount], shader_differ[StageCount];
} tally;
u64 last_report = 0;
int examples_left = 8;
// The last tag read: its number and where the packet after it starts.
u64 tag_number = 0;
const void* tag_next = nullptr;

u64 NowMs() {
    return u64(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

// Direct3D 11's topology order without gaps (annotations: CgDrawState.topology) to GCN's.
u32 ExpectedPrimitive(u32 topology) {
    constexpr u32 map[] = {0, 1, 2, 3, 4, 6, 10, 11, 12, 13};
    return topology < 10 ? map[topology] : u32(AmdGpu::PrimitiveType::PatchPrimitive);
}

void Compare(Field field, bool equal, const char* what, u64 expected, u64 actual) {
    ++tally.compared[field];
    if (!equal) {
        ++tally.differ[field];
        if (examples_left > 0) {
            --examples_left;
            std::printf("Render boundary: %s differs: the draw state says %#llx, the packets %#llx\n", what,
                        static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual));
        }
    }
}

void Report(u64 now) {
    std::printf("Render boundary: method calls");
    for (u32 m = 0; m < Methods; ++m) {
        std::printf(" %s %llu", method_names[m], static_cast<unsigned long long>(method_calls[m].load()));
    }
    std::printf("; tagged %llu, no room to tag %llu, libGnm draws outside %llu\n",
                static_cast<unsigned long long>(filed.load()), static_cast<unsigned long long>(no_room.load()),
                static_cast<unsigned long long>(gnm_without_method.load()));
    std::printf("Render boundary: draw packets with a record %llu, without %llu; agree/compared:",
                static_cast<unsigned long long>(tally.compared[Found]),
                static_cast<unsigned long long>(tally.compared[Missing]));
    for (int f = TopologyField; f < Fields; ++f) {
        std::printf(" %s %llu/%llu", field_names[f], static_cast<unsigned long long>(tally.compared[f] - tally.differ[f]),
                    static_cast<unsigned long long>(tally.compared[f]));
    }
    for (int s = 0; s < StageCount; ++s) {
        std::printf(" %s shader %llu/%llu", stage_names[s],
                    static_cast<unsigned long long>(tally.shader_compared[s] - tally.shader_differ[s]),
                    static_cast<unsigned long long>(tally.shader_compared[s]));
    }
    std::printf("\n");
    std::fflush(stdout);
    last_report = now;
}
} // namespace

bool Observing() {
    return observing;
}

void NoteTag(const void* nop) {
    tag_number = static_cast<const u32*>(nop)[2];
    tag_next = static_cast<const u32*>(nop) + 3;
}

void ObserveDraw(const void* packet, const AmdGpu::Regs& regs, bool indexed, u32 index_offset, bool indirect) {
    Record d;
    if (packet != tag_next || !Find(tag_number, d)) {
        ++tally.compared[Missing];
    } else {
        ++tally.compared[Found];
        const u32 primitive = u32(regs.primitive_type);
        Compare(TopologyField, ExpectedPrimitive(d.topology) == primitive, "topology", d.topology, primitive);
        if (indexed) {
            const u32 type = u32(regs.index_buffer_type.index_type);
            Compare(IndexTypeField, (d.index_format == 0x2a) == (type == 1), "index type", d.index_format, type);
        }
        if (!indirect) {
            Compare(CountField, d.count == regs.num_indices, "count", d.count, regs.num_indices);
            if (d.method == DrawInstanced || d.method == DrawIndexedInstanced) {
                Compare(InstancesField, d.instances == regs.num_instances.num_instances, "instances", d.instances,
                        regs.num_instances.num_instances);
            }
        }
        if (indexed && !indirect && d.index_start) {
            const u64 size = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index32 ? 4 : 2;
            const u64 start = regs.index_base_address.Address<u64>() + u64(index_offset) * size;
            Compare(IndexStartField, d.index_start == start, "index start", d.index_start, start);
        }
        // Where each of the draw state's stages runs on GCN (annotations: CgStage).
        const auto& stages = regs.stage_enable;
        const bool tessellated = stages.hs_en, geometry = stages.gs_en;
        const AmdGpu::ShaderProgram* runs_on[StageCount] = {
            tessellated ? &regs.ls_program : geometry ? &regs.es_program : &regs.vs_program,
            &regs.hs_program,
            geometry ? &regs.es_program : &regs.vs_program,
            &regs.gs_program,
            &regs.ps_program,
        };
        for (int s = 0; s < StageCount; ++s) {
            if (!d.shader[s] || ((s == Hull || s == Domain) && !tessellated) || (s == Geometry && !geometry)) {
                continue;
            }
            const u64 actual = runs_on[s]->Address<u64>();
            ++tally.shader_compared[s];
            if (actual != d.shader[s]) {
                ++tally.shader_differ[s];
                if (examples_left > 0) {
                    --examples_left;
                    std::printf("Render boundary: %s shader differs: the draw state says %#llx, the packets %#llx\n",
                                stage_names[s], static_cast<unsigned long long>(d.shader[s] - image_base),
                                static_cast<unsigned long long>(actual - image_base));
                }
            }
        }
    }
    const u64 now = NowMs();
    if (now - last_report >= 5000) {
        Report(now);
    }
}

void PatchImage(unsigned char* image, std::uint64_t) {
    const char* env = std::getenv("BB_RENDER_BOUNDARY");
    if (!env || std::strcmp(env, "observe") != 0) {
        return;
    }
    const char* census = std::getenv("BB_DRAW_CENSUS");
    if (census && census[0] == '1') {
        std::printf("Render boundary: not with BB_DRAW_CENSUS=1 (the same hooks)\n");
        return;
    }
    image_base = reinterpret_cast<u64>(image);
    slots = new Slot[Slots]();
    // The prologues the draw census checks (bbport_draw_census.cpp).
    static constexpr u8 r15[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57};
    static constexpr u8 r14[] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56};
    static constexpr u8 rbx[] = {0x55, 0x48, 0x89, 0xe5, 0x53};
    static constexpr u8 cmp_esi[] = {0x55, 0x48, 0x89, 0xe5, 0x83, 0xfe, 0xff};
    static constexpr u8 xor_eax[] = {0x55, 0x48, 0x89, 0xe5, 0x31, 0xc0};
    auto install = [&](u64 offset, u64 tag, Core::GuestCodeHook hook) {
        void* at = image + offset;
        for (const auto& bytes : {std::span<const u8>(r15), std::span<const u8>(r14), std::span<const u8>(rbx),
                                  std::span<const u8>(cmp_esi), std::span<const u8>(xor_eax)}) {
            if (Core::InstallGuestCodeHook(at, bytes, tag, hook)) {
                return true;
            }
        }
        std::printf("Render boundary: no hook at %#llx\n", static_cast<unsigned long long>(offset));
        return false;
    };
    int installed = 0, wanted = 0;
    for (u32 m = 0; m < Methods; ++m, ++wanted) {
        installed += install(method_offsets[m], m, OnMethod);
    }
    for (const u64 offset : gnm_offsets) {
        installed += install(offset, GnmTag, OnGnmDraw);
        ++wanted;
    }
    observing = installed == wanted;
    std::printf("Render boundary: observing draws (%d of %d hooks)\n", installed, wanted);
    last_report = NowMs();
}

} // namespace BbRenderBoundary
