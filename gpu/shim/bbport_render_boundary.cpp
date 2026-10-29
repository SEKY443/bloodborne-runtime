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
#include "bbport_guest_call.h"
#include "bbport_guest_code_hooks.h"
#include "cpu/bbcpu.h"

#include "video_core/amdgpu/regs.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_draw_prep.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bit>
#include <bitset>
#include <span>
#include <unordered_map>
#include <vector>

namespace BbRenderBoundary {
namespace {
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u8 = std::uint8_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

enum Method : u32 { Draw, DrawIndexed, DrawInstanced, DrawIndexedInstanced, DrawIndirect, DrawIndexedIndirect, Methods };
constexpr const char* method_names[Methods] = {"draw", "drawIndexed", "drawInstanced", "drawIndexedInstanced",
                                               "drawIndirect", "drawIndexedIndirect"};
// CG::GraphicsContext's draw methods and libGnm's draw functions (annotations/functions.tsv, 1.09).
constexpr u64 method_offsets[Methods] = {0x21696c0, 0x2169780, 0x2169840, 0x2169900, 0x21699d0, 0x2169ab0};
constexpr u64 method_sizes[Methods] = {180, 189, 181, 194, 223, 226};
constexpr u64 gnm_offsets[] = {
    0x1074360, // Gnm::DrawCommandBuffer::drawIndex
    0x10741b0, // drawIndexAuto
    0x1074420, // drawIndexOffset
    0x10744d0, // drawIndirect
    0x10746f0, // drawIndexIndirect
};
constexpr u64 GnmTag = 0x100;
constexpr u64 MeshDraw = 0x21d0640, MeshDrawSize = 2621; // CG::MeshRenderer::drawMesh

// CgDrawState (annotations/structs.h).
constexpr u64 Topology = 0x8, IndexBuffer = 0x10, IndexFormat = 0x18, IndexOffset = 0x1c, Stages = 0x598,
              StageSize = 0x4f8, StageShader = 0x4f0, ShaderCode = 0x108, BufferAddress = 0x20, BufferManaged = 0x40,
              Rasterizer = 0x240, Viewports = 0x248, Scissors = 0x3c8, Blend = 0x4d0, DepthStencil = 0x4d8,
              RenderTargets = 0x4e0, DepthTarget = 0x520, BlendColor = 0x568, DepthBounds = 0x578,
              StencilReference = 0x584, ScissorEnable = 0x15, VertexBuffers = 0x20, VertexStrides = 0x120,
              VertexOffsets = 0x1a0, StageSamplers = 0x0, StageTextures = 0x80, StageTextureSlots = 128,
              StageConstantBuffers = 0x480, TextureSharp = 0x10, ConstantBufferSize = 0x2c,
              ConstantBufferHandle = 0x18;
// The context's buffer manager, and its lookup of a buffer's GPU address from the buffer's handle
// (what CG::ResourceBinder::commitGraphics calls before initAsConstantBuffer).
constexpr u64 ContextBufferManager = 0xb3c0, BufferManagerAddressOf = 0x26ae840;
// Index and vertex buffers marked managed: another table of the context's, the same handle offset
// (CG::InputAssemblyState::apply).
constexpr u64 ContextGeometryManager = 0xb6f8, GeometryManagerAddressOf = 0x26d0ff0, BufferHandle = 0x18;
// A render or depth target object holds libGnm's RenderTarget or DepthRenderTarget at +0x10
// (CG::OutputMergerState::apply passes that to setRenderTarget and setDepthRenderTarget); their
// CB_COLOR_BASE is the first register, DB_Z_READ_BASE the third (the address shifted by 8).
constexpr u64 GnmTarget = 0x10, ColorBase = 0x0, DepthReadBase = 0x8;
enum Stage { Vertex, Hull, Domain, Geometry, Pixel, StageCount };
constexpr const char* stage_names[StageCount] = {"vertex", "hull", "domain", "geometry", "pixel"};
// libGnm's command buffer: the end of its space and the write pointer.
constexpr u64 CommandEnd = 0x8, CommandPointer = 0x10;
// The tag: a type-3 NOP with two payload dwords, the magic and the record's number.
constexpr u32 NopTwoDwords = 0xc0011000;

struct Record {
    u64 key; // the record's number
    u32 method, topology, index_format, count, instances, base_vertex;
    bool mesh; // made by CG::MeshRenderer::drawMesh
    u64 index_start; // the first index's address; 0 when unknown (a managed buffer)
    u64 shader[StageCount];
    // The output state; a block the draw state leaves NULL (the engine's defaults) is not compared.
    bool has_blend, has_depth_stencil, has_rasterizer, scissor_enabled;
    u32 target_mask, blend[8], depth_stencil_control, stencil_op_control, clip_control, primitive_setup;
    u8 stencil_reference;
    float viewport[6], blend_color[4], depth_bounds[2];
    s32 scissor[4];
    u64 color_target[8], depth_target; // the targets' addresses; 0 for none
    // The resources by slot: per stage the texture and sampler objects (0: an empty slot; their
    // T#s and S#s are read on the GPU thread, as the GPU reads the binder's tables) and each
    // constant buffer's size and address; the vertex buffers' strides and addresses with their
    // offsets. Buffer addresses the engine's managers place are asked for at libGnm's draw.
    struct Resources {
        u64 texture[StageTextureSlots], sampler[16];
        u32 constant_buffer_size[14];
        u64 constant_buffer_address[14];
    } stage[StageCount];
    u32 vertex_stride[32];
    u64 vertex_address[32]; // 0: an empty slot
    u64 input_layout;       // the draw state's (+0x0): it decides which slot each attribute reads
};

struct Pending {
    bool valid;
    Record record;
    u64 context;
    u64 constant_buffer[StageCount][14]; // the CgConstantBuffer objects, by slot
    u64 managed_index;                   // the managed index buffer's CgBuffer, and its offset
    u64 managed_index_offset;
    u64 managed_vertex[32];              // the managed vertex buffers' CgBuffers, by slot
};
thread_local Pending pending;

// Records by number, in a ring; a sequence word per slot (odd while written).
constexpr std::size_t Slots = 1 << 13;
struct Slot {
    std::atomic<u32> sequence;
    Record record;
};
Slot* slots = nullptr;
bool observing = false;
bool take_mesh_draws = false; // BB_RENDER_BOUNDARY=mesh
/// N2.3: BB_RENDER_BOUNDARY=flat-check (the worker also fills the flattened user data of mesh
/// draws from the draw state and compares the sharps it gives) or flat (the fill replaces the
/// walker).
enum FlatMode { FlatOff, FlatCheck, FlatFill };
FlatMode flat_mode = FlatOff;
u64 image_base = 0;
std::atomic<u64> method_calls[Methods], filed{0}, gnm_without_method{0}, gnm_nested{0}, no_room{0}, next_number{1};

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

u64 Hash(const void* data, std::size_t size) {
    u64 hash = 0xcbf29ce484222325ull, word;
    for (std::size_t i = 0; i < size; i += 8) {
        std::memcpy(&word, static_cast<const u8*>(data) + i, 8);
        hash = (hash ^ word) * 0x100000001b3ull;
        hash ^= hash >> 29;
    }
    return hash;
}

// A slot's value: a 32-bit hash, never 0 (0 marks an empty slot).
u32 SlotHash(const void* data, std::size_t size) {
    const u64 hash = Hash(data, size);
    return u32(hash ^ hash >> 32) | 1;
}

void ReadResources(u64 state, Record& d) {
    for (int s = 0; s < StageCount; ++s) {
        const u64 stage = state + Stages + s * StageSize;
        auto& r = d.stage[s];
        if (!Read<u64>(stage + StageShader)) {
            continue;
        }
        for (u32 i = 0; i < StageTextureSlots; ++i) {
            r.texture[i] = Read<u64>(stage + StageTextures + 8 * i);
        }
        for (u32 i = 0; i < 16; ++i) {
            r.sampler[i] = Read<u64>(stage + StageSamplers + 8 * i);
        }
        for (u32 i = 0; i < 14; ++i) {
            const u64 buffer = Read<u64>(stage + StageConstantBuffers + 8 * i);
            pending.constant_buffer[s][i] = buffer;
            r.constant_buffer_size[i] = buffer ? Read<u32>(buffer + ConstantBufferSize) : 0;
        }
    }
    d.input_layout = Read<u64>(state);
    for (u32 i = 0; i < 32; ++i) {
        pending.managed_vertex[i] = 0;
        if (const u64 buffer = Read<u64>(state + VertexBuffers + 8 * i)) {
            d.vertex_stride[i] = Read<u32>(state + VertexStrides + 4 * i);
            d.vertex_address[i] = Read<u32>(state + VertexOffsets + 4 * i);
            if (!Read<u8>(buffer + BufferManaged)) {
                d.vertex_address[i] += Read<u64>(buffer + BufferAddress);
            } else {
                pending.managed_vertex[i] = buffer; // the offset now, the address at libGnm's draw
            }
        }
    }
}

void __attribute__((sysv_abi)) OnMethod(u64 tag, const Core::GuestRegisterSnapshot* r) {
    method_calls[tag].fetch_add(1, std::memory_order_relaxed);
    Record& d = pending.record;
    d = {};
    d.method = u32(tag);
    pending.context = r->rdi;
    // Called from the mesh draw: its return address is in CG::MeshRenderer::drawMesh. From the
    // recompiled library the hooked method runs in the translator, whose frame address stands in
    // that slot instead: bbcpu_guest_return gives the caller's back.
    d.mesh = bbcpu_guest_return(Read<u64>(r->rsp)) - image_base - MeshDraw < MeshDrawSize;
    const u64 state = r->rsi;
    const u32 a = u32(r->rdx), b = u32(r->rcx), c = u32(r->r8);
    u32 first_index = 0;
    switch (tag) {
    case Draw: d.count = a, d.instances = 1, d.base_vertex = b; break;
    case DrawIndexed: d.count = a, first_index = b, d.instances = 1, d.base_vertex = c; break;
    case DrawInstanced: d.count = a, d.instances = b, d.base_vertex = c; break;
    case DrawIndexedInstanced: d.count = a, d.instances = b, first_index = c, d.base_vertex = u32(r->r9); break;
    default: break; // indirect: the counts are in memory
    }
    d.topology = Read<u32>(state + Topology);
    d.index_format = Read<u32>(state + IndexFormat);
    if (tag == DrawIndexed || tag == DrawIndexedInstanced) {
        const u64 buffer = Read<u64>(state + IndexBuffer);
        const u64 size = d.index_format == 0x2a ? 4 : 2;
        pending.managed_index = 0;
        if (buffer && !Read<u8>(buffer + BufferManaged)) {
            d.index_start = Read<u64>(buffer + BufferAddress) + Read<u32>(state + IndexOffset) + first_index * size;
        } else if (buffer) {
            pending.managed_index = buffer; // its address is asked for at libGnm's draw
            pending.managed_index_offset = Read<u32>(state + IndexOffset) + first_index * size;
        }
    }
    for (int s = 0; s < StageCount; ++s) {
        const u64 shader = Read<u64>(state + Stages + s * StageSize + StageShader);
        d.shader[s] = shader ? Read<u64>(shader + ShaderCode) : 0;
    }
    if (const u64 blend = Read<u64>(state + Blend)) {
        d.has_blend = true;
        d.target_mask = Read<u32>(blend);
        std::memcpy(d.blend, reinterpret_cast<const void*>(blend + 4), sizeof(d.blend));
    }
    if (const u64 depth_stencil = Read<u64>(state + DepthStencil)) {
        d.has_depth_stencil = true;
        d.depth_stencil_control = Read<u32>(depth_stencil);
        d.stencil_op_control = Read<u32>(depth_stencil + 4);
    }
    if (const u64 rasterizer = Read<u64>(state + Rasterizer)) {
        d.has_rasterizer = true;
        d.clip_control = Read<u32>(rasterizer);
        d.primitive_setup = Read<u32>(rasterizer + 4);
        // setScanModeControl's second argument: the viewport scissor applies only when it is set.
        d.scissor_enabled = Read<u8>(rasterizer + ScissorEnable) != 0;
    }
    d.stencil_reference = Read<u8>(state + StencilReference);
    std::memcpy(d.viewport, reinterpret_cast<const void*>(state + Viewports), sizeof(d.viewport));
    std::memcpy(d.scissor, reinterpret_cast<const void*>(state + Scissors), sizeof(d.scissor));
    std::memcpy(d.blend_color, reinterpret_cast<const void*>(state + BlendColor), sizeof(d.blend_color));
    std::memcpy(d.depth_bounds, reinterpret_cast<const void*>(state + DepthBounds), sizeof(d.depth_bounds));
    for (int i = 0; i < 8; ++i) {
        const u64 target = Read<u64>(state + RenderTargets + 8 * i);
        d.color_target[i] = target ? u64(Read<u32>(target + GnmTarget + ColorBase)) << 8 : 0;
    }
    const u64 depth = Read<u64>(state + DepthTarget);
    d.depth_target = depth ? u64(Read<u32>(depth + GnmTarget + DepthReadBase)) << 8 : 0;
    ReadResources(state, d);
    pending.valid = true;
}

void __attribute__((sysv_abi)) OnGnmDraw(u64, const Core::GuestRegisterSnapshot* r) {
    if (!pending.valid) {
        gnm_without_method.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Only the method's own draw takes the record: a draw the method makes through something else
    // first (a fast-clear resolve when a render target changes) returns elsewhere.
    const u64 back = Read<u64>(r->rsp) - image_base, method = pending.record.method;
    if (back - method_offsets[method] >= method_sizes[method]) {
        gnm_nested.fetch_add(1, std::memory_order_relaxed);
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
    // The binder has run: the buffer manager knows this draw's constant buffers.
    const u64 manager = pending.context + ContextBufferManager;
    for (int s = 0; s < StageCount; ++s) {
        for (u32 i = 0; i < 14; ++i) {
            if (const u64 buffer = pending.constant_buffer[s][i]) {
                pending.record.stage[s].constant_buffer_address[i] = BbGuest::Call<u64>(
                    image_base + BufferManagerAddressOf, manager, buffer + ConstantBufferHandle);
            }
        }
    }
    const u64 geometry = pending.context + ContextGeometryManager;
    if (pending.managed_index) {
        pending.record.index_start =
            BbGuest::Call<u64>(image_base + GeometryManagerAddressOf, geometry, pending.managed_index + BufferHandle) +
            pending.managed_index_offset;
    }
    for (u32 n = 0; n < 32; ++n) {
        if (pending.managed_vertex[n]) {
            pending.record.vertex_address[n] +=
                BbGuest::Call<u64>(image_base + GeometryManagerAddressOf, geometry, pending.managed_vertex[n] + BufferHandle);
        }
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
enum Field {
    Found, Missing, TopologyField, IndexTypeField, CountField, InstancesField, IndexStartField,
    TargetMaskField, BlendField, DepthStencilField, StencilOpField, ClipField, PrimitiveSetupField,
    StencilReferenceField, ViewportField, ScissorField, BlendColorField, DepthBoundsField, ColorTargetField,
    DepthTargetField, Fields
};
constexpr const char* field_names[Fields] = {
    "found", "missing", "topology", "index type", "count", "instances", "index start", "target mask", "blend",
    "depth-stencil", "stencil ops", "clip", "primitive setup", "stencil reference", "viewport", "scissor",
    "blend colour", "depth bounds", "colour target", "depth target"};
struct Tally {
    u64 compared[Fields], differ[Fields];
    u64 shader_compared[StageCount], shader_differ[StageCount];
} tally;
u64 last_report = 0;
// The first differences printed: three per field, three for the shaders.
int examples_printed[Fields] = {}, shader_examples_left = 3;
// The last tag read: its number and where the packet after it starts.
u64 tag_number = 0;
const void* tag_next = nullptr;

u64 NowMs() {
    return u64(std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count());
}

template <typename T>
u32 Raw(const T& value) {
    static_assert(sizeof(T) == 4);
    u32 raw;
    std::memcpy(&raw, &value, 4);
    return raw;
}

bool Near(float a, float b) {
    return a - b < 1e-3f && b - a < 1e-3f;
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
        if (examples_printed[field] < 3) {
            ++examples_printed[field];
            std::printf("Render boundary: %s differs: the draw state says %#llx, the packets %#llx\n", what,
                        static_cast<unsigned long long>(expected), static_cast<unsigned long long>(actual));
        }
    }
}

void CompareOutputs(const Record& d, const AmdGpu::Regs& regs) {
    if (d.has_blend) {
        Compare(TargetMaskField, d.target_mask == Raw(regs.color_target_mask), "target mask", d.target_mask,
                Raw(regs.color_target_mask));
        for (int i = 0; i < 8; ++i) {
            if (d.color_target[i]) {
                Compare(BlendField, d.blend[i] == Raw(regs.blend_control[i]), "blend", d.blend[i],
                        Raw(regs.blend_control[i]));
            }
        }
    }
    if (d.has_depth_stencil) {
        Compare(DepthStencilField, d.depth_stencil_control == Raw(regs.depth_control), "depth-stencil",
                d.depth_stencil_control, Raw(regs.depth_control));
        Compare(StencilOpField, d.stencil_op_control == Raw(regs.stencil_control), "stencil ops",
                d.stencil_op_control, Raw(regs.stencil_control));
    }
    if (d.has_rasterizer) {
        Compare(ClipField, d.clip_control == Raw(regs.clipper_control), "clip", d.clip_control,
                Raw(regs.clipper_control));
        Compare(PrimitiveSetupField, d.primitive_setup == Raw(regs.polygon_control), "primitive setup",
                d.primitive_setup, Raw(regs.polygon_control));
    }
    Compare(StencilReferenceField, d.stencil_reference == regs.stencil_ref_front.stencil_test_val,
            "stencil reference", d.stencil_reference, regs.stencil_ref_front.stencil_test_val);
    // Viewport 0 (x, y, width, height, min and max depth) against the scale the hardware gets.
    const auto& v = regs.viewports[0];
    const bool viewport = Near(v.xscale, d.viewport[2] / 2) && Near(v.yscale < 0 ? -v.yscale : v.yscale, d.viewport[3] / 2) &&
                          Near(v.xoffset, d.viewport[0] + d.viewport[2] / 2) &&
                          Near(v.yoffset, d.viewport[1] + d.viewport[3] / 2);
    Compare(ViewportField, viewport, "viewport width (x100)", u64(d.viewport[2] * 100), u64(v.xscale * 200));
    if (d.scissor_enabled) {
        const auto& s = regs.viewport_scissors[0];
        const bool scissor = s.top_left_x == u32(d.scissor[0] < 0 ? 0 : d.scissor[0]) &&
                             s.top_left_y == u32(d.scissor[1] < 0 ? 0 : d.scissor[1]) &&
                             s.bottom_right_x == u32(d.scissor[2] < 0 ? 0 : d.scissor[2]) &&
                             s.bottom_right_y == u32(d.scissor[3] < 0 ? 0 : d.scissor[3]);
        Compare(ScissorField, scissor, "scissor right", u64(d.scissor[2]), s.bottom_right_x);
    }
    bool blend_color = true;
    for (int i = 0; i < 4; ++i) {
        blend_color = blend_color && d.blend_color[i] == regs.blend_constants[i];
    }
    Compare(BlendColorField, blend_color, "blend colour red (x1000)", u64(d.blend_color[0] * 1000),
            u64(regs.blend_constants[0] * 1000));
    Compare(DepthBoundsField, d.depth_bounds[0] == regs.depth_bounds_min && d.depth_bounds[1] == regs.depth_bounds_max,
            "depth bounds max (x1000)", u64(d.depth_bounds[1] * 1000), u64(regs.depth_bounds_max * 1000));
    for (int i = 0; i < 8; ++i) {
        if (d.color_target[i]) {
            const u64 actual = regs.color_buffers[i].Address();
            Compare(ColorTargetField, d.color_target[i] == actual, "colour target", d.color_target[i], actual);
        }
    }
    if (d.depth_target) {
        const u64 actual = regs.depth_buffer.DepthAddress();
        Compare(DepthTargetField, d.depth_target == actual, "depth target", d.depth_target, actual);
    }
}

// What the shaders read (the draw-preparation worker's sharps) against the draw state's resources.
// What the shaders read (the draw-preparation worker's sharps) against the draw state's slots. For
// each shader the slots a resource could come from are intersected over its draws: one slot left
// is the resource's slot; none means it does not come from a fixed slot.
enum Resource { Texture, Sampler, ConstantBuffer, VertexBuffer, Resources };
constexpr const char* resource_names[Resources] = {"textures", "samplers", "constant buffers", "vertex attributes"};
struct ResourceTally {
    u64 read[Resources], found[Resources], unprepared, prepared;
} resources;

struct SlotMap {
    std::vector<std::bitset<StageTextureSlots>> images;
    std::vector<std::bitset<16>> samplers;
    std::vector<std::bitset<14>> buffers; // constant buffers; the null buffer keeps every bit
    std::vector<std::bitset<32>> attributes;
    std::vector<s64> attribute_offsets; // the V#'s address less its slot's; INT64_MIN: not fixed
    bool started = false;
    // N2.3: per constant buffer its V#'s dwords as the walker left them in the flattened user data
    // (the draw state gives only the address and size), and the draws the map was learnt from.
    std::vector<std::array<u32, 4>> buffer_raw;
    std::vector<bool> buffer_raw_known;
    u64 draws = 0;
};
struct PairHash {
    std::size_t operator()(const std::pair<const void*, u64>& key) const {
        return std::hash<const void*>()(key.first) ^ std::hash<u64>()(key.second) * 0x9e3779b97f4a7c15ull;
    }
};
// By the shader program; vertex attributes by the vertex program and the draw state's input layout.
std::unordered_map<std::pair<const void*, u64>, SlotMap, PairHash> slot_maps;

template <std::size_t N>
void Narrow(std::vector<std::bitset<N>>& list, std::size_t k, const std::bitset<N>& candidates, bool started) {
    if (list.size() <= k) {
        list.resize(k + 1);
        list[k].set();
    }
    if (!started) {
        list[k] = candidates;
    } else {
        list[k] &= candidates;
    }
}

template <std::size_t N>
u32 FirstSlot(const std::bitset<N>& bits) {
    for (u32 i = 0; i < N; ++i) {
        if (bits[i]) {
            return i;
        }
    }
    return N;
}

// BB_RENDER_BOUNDARY=mesh: a mesh draw's resources from the draw state, through the slot maps, in
// place of the ones the draw-preparation worker read from the user data. The arrays are the
// submission's; the GPU thread binds from them after this.
struct Taken {
    u64 draws, resources, differed;
} taken;

static_assert(sizeof(AmdGpu::Sampler) == 16 && sizeof(AmdGpu::Image) == 32, "the S# and T# copied from the engine");

template <typename T>
void Take(const T& from, const T& into_const) {
    T& into = const_cast<T&>(into_const);
    ++taken.resources;
    if (std::memcmp(&from, &into, sizeof(T)) != 0) {
        ++taken.differed;
        std::memcpy(&into, &from, sizeof(T));
    }
}

void TakeResources(const Vulkan::PreparedStage& p, const Record::Resources& r, const SlotMap& map) {
    for (u32 k = 0; k < p.num_images && k < map.images.size(); ++k) {
        const u32 slot = FirstSlot(map.images[k]);
        if (slot < StageTextureSlots && r.texture[slot]) {
            AmdGpu::Image sharp;
            std::memcpy(&sharp, reinterpret_cast<const void*>(r.texture[slot] + TextureSharp), sizeof(sharp));
            Take(sharp, p.image_sharps[k]);
        }
    }
    for (u32 k = 0; k < p.num_samplers && k < map.samplers.size(); ++k) {
        const u32 slot = FirstSlot(map.samplers[k]);
        if (slot < 16 && r.sampler[slot]) {
            AmdGpu::Sampler sharp;
            std::memcpy(&sharp, reinterpret_cast<const void*>(r.sampler[slot]), sizeof(sharp));
            Take(sharp, p.sampler_sharps[k]);
        }
    }
    for (u32 k = 0; k < p.num_buffers && k < map.buffers.size(); ++k) {
        const AmdGpu::Buffer& worker = p.buffer_sharps[k];
        const u32 slot = FirstSlot(map.buffers[k]);
        if (map.buffers[k].all() || slot >= 14 || !r.constant_buffer_address[slot]) {
            continue; // the null buffer, or no slot
        }
        AmdGpu::Buffer sharp = worker; // initAsConstantBuffer's other fields
        sharp.base_address = r.constant_buffer_address[slot];
        sharp.num_records = (r.constant_buffer_size[slot] + 15) / 16;
        Take(sharp, worker);
    }
}

void TakeVertexBuffers(const Vulkan::PreparedDraw& prepared, const Record& d, const SlotMap& map) {
    for (u32 k = 0; k < prepared.vertex.count && k < map.attributes.size(); ++k) {
        const u32 slot = FirstSlot(map.attributes[k]);
        const s64 offset = k < map.attribute_offsets.size() ? map.attribute_offsets[k] : INT64_MIN;
        if (slot >= 32 || offset == INT64_MIN || !d.vertex_address[slot]) {
            continue;
        }
        const AmdGpu::Buffer& worker = prepared.vertex.buffers[k];
        AmdGpu::Buffer sharp = worker; // the format and record count are the attribute's
        sharp.base_address = d.vertex_address[slot] + offset;
        sharp.stride = d.vertex_stride[slot];
        Take(sharp, worker);
    }
}

// N2's inventory: per shader program, how its resources reach it. A sharp dword comes from a
// user-data register (flat offset below 16), from a table the walker flattened (16 and above) or
// is a constant of the shader's.
struct ProgramInventory {
    Shader::HwStage stage;
    u64 hash, draws = 0, mesh_draws = 0;
    u32 images = 0, samplers = 0, buffers = 0, special_buffers = 0;
    u32 from_registers = 0, from_tables = 0, constant = 0; // sharps, by where most of their dwords come from
    u32 registers = 0, registers_for_sharps = 0, flattened_dwords = 0;
    bool dynamic_constants = false, immediate_constants = false;
};
std::unordered_map<const void*, ProgramInventory> inventory;

template <typename Fetch>
void Classify(const Fetch& fetch, ProgramInventory& e, u32& sharp_registers) {
    u32 registers = 0, tables = 0, constants = 0;
    for (std::size_t i = 0; i < fetch.offsets.size(); ++i) {
        if (!(fetch.load_mask >> i & 1)) {
            ++constants;
        } else if (fetch.offsets[i] < 16) {
            ++registers;
            sharp_registers |= 1u << fetch.offsets[i];
        } else {
            ++tables;
        }
    }
    (registers >= tables && registers >= constants ? e.from_registers : tables >= constants ? e.from_tables : e.constant) += 1;
}

ProgramInventory& Inventory(const Vulkan::PreparedStage& p) {
    auto [it, fresh] = inventory.try_emplace(p.program);
    ProgramInventory& e = it->second;
    if (fresh) {
        const Shader::Info& info = p.program->info;
        e.stage = p.hw_stage;
        e.hash = info.pgm_hash;
        u32 sharp_registers = 0;
        for (const auto& image : info.images) {
            ++e.images;
            Classify(image.sharp_fetch, e, sharp_registers);
        }
        for (const auto& sampler : info.samplers) {
            ++e.samplers;
            Classify(sampler.sharp_fetch, e, sharp_registers);
        }
        for (const auto& buffer : info.buffers) {
            if (buffer.IsSpecial()) {
                ++e.special_buffers;
                continue;
            }
            ++e.buffers;
            Classify(buffer.sharp_fetch, e, sharp_registers);
        }
        e.registers = info.ud_mask.NumRegs();
        e.registers_for_sharps = std::popcount(sharp_registers);
        e.flattened_dwords = info.srt_info.flattened_bufsize_dw;
        const u32 readconst = u32(info.readconst_types);
        e.immediate_constants = readconst & u32(Shader::Info::ReadConstType::Immediate);
        e.dynamic_constants = readconst & u32(Shader::Info::ReadConstType::Dynamic);
    }
    return e;
}

void ReportInventory() {
    u64 programs = 0, draws = 0, mesh = 0, sharps[3] = {}, special = 0, extra_registers = 0, dynamic = 0, immediate = 0;
    for (const auto& [program, e] : inventory) {
        ++programs;
        draws += e.draws;
        mesh += e.mesh_draws;
        sharps[0] += e.from_registers, sharps[1] += e.from_tables, sharps[2] += e.constant;
        special += e.special_buffers != 0;
        extra_registers += e.registers > e.registers_for_sharps;
        dynamic += e.dynamic_constants;
        immediate += e.immediate_constants;
    }
    std::printf("Render boundary: inventory: %llu programs in %llu stage draws (%llu of mesh draws); sharps from "
                "user-data registers %llu, from tables %llu, constant %llu; programs reading user-data registers "
                "beyond their sharps %llu, with special buffers %llu, constant reads immediate %llu, dynamic %llu\n",
                static_cast<unsigned long long>(programs), static_cast<unsigned long long>(draws),
                static_cast<unsigned long long>(mesh), static_cast<unsigned long long>(sharps[0]),
                static_cast<unsigned long long>(sharps[1]), static_cast<unsigned long long>(sharps[2]),
                static_cast<unsigned long long>(extra_registers), static_cast<unsigned long long>(special),
                static_cast<unsigned long long>(immediate), static_cast<unsigned long long>(dynamic));
    // Per program, for the plan (BB_RENDER_BOUNDARY_INVENTORY=path): counts only.
    static const char* path = std::getenv("BB_RENDER_BOUNDARY_INVENTORY");
    if (path) {
        if (FILE* f = std::fopen(path, "w")) {
            std::fprintf(f, "hash,stage,draws,mesh_draws,images,samplers,buffers,special_buffers,sharps_from_registers,"
                            "sharps_from_tables,sharps_constant,registers,registers_for_sharps,flattened_dwords,"
                            "immediate_constants,dynamic_constants\n");
            for (const auto& [program, e] : inventory) {
                std::fprintf(f, "%016llx,%u,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d\n",
                             static_cast<unsigned long long>(e.hash), u32(e.stage),
                             static_cast<unsigned long long>(e.draws), static_cast<unsigned long long>(e.mesh_draws),
                             e.images, e.samplers, e.buffers, e.special_buffers, e.from_registers, e.from_tables,
                             e.constant, e.registers, e.registers_for_sharps, e.flattened_dwords,
                             e.immediate_constants, e.dynamic_constants);
            }
            std::fclose(f);
        }
    }
}

void CompareResources(const Record& d, const AmdGpu::Regs& regs, const Vulkan::PreparedDraw* prepared,
                      u64 reg_checksum) {
    if (!prepared || prepared->state.load(std::memory_order_acquire) != Vulkan::PreparedDraw::Ready ||
        prepared->reg_checksum != reg_checksum) {
        ++resources.unprepared;
        return;
    }
    ++resources.prepared;
    taken.draws += take_mesh_draws && d.mesh;
    const bool tessellated = regs.stage_enable.hs_en, geometry = regs.stage_enable.gs_en;
    const Vulkan::PreparedStage* vertex_stage = nullptr;
    for (u32 i = 0; i < prepared->num_stages; ++i) {
        const Vulkan::PreparedStage& p = prepared->stages[i];
        int s = -1;
        switch (p.hw_stage) {
        case Shader::HwStage::Fragment: s = Pixel; break;
        case Shader::HwStage::Local: s = Vertex; break;
        case Shader::HwStage::Hull: s = Hull; break;
        case Shader::HwStage::Geometry: s = Geometry; break;
        case Shader::HwStage::Export: s = tessellated ? Domain : Vertex; break;
        case Shader::HwStage::Vertex: s = geometry ? -1 : tessellated ? Domain : Vertex; break; // -1: GS copy shader
        default: break;
        }
        if (s < 0) {
            continue;
        }
        if (s == Vertex) {
            vertex_stage = &p;
        }
        ProgramInventory& entry = Inventory(p);
        ++entry.draws;
        entry.mesh_draws += d.mesh;
        const auto& r = d.stage[s];
        u32 texture_hash[StageTextureSlots], sampler_hash[16];
        for (u32 slot = 0; slot < StageTextureSlots; ++slot) {
            texture_hash[slot] = r.texture[slot]
                                     ? SlotHash(reinterpret_cast<const void*>(r.texture[slot] + TextureSharp),
                                                sizeof(AmdGpu::Image))
                                     : 0;
        }
        for (u32 slot = 0; slot < 16; ++slot) {
            sampler_hash[slot] = r.sampler[slot] ? SlotHash(reinterpret_cast<const void*>(r.sampler[slot]), 16) : 0;
        }
        SlotMap& map = slot_maps[{p.program, 0}];
        for (u32 k = 0; k < p.num_images; ++k) {
            const u32 hash = SlotHash(&p.image_sharps[k], sizeof(AmdGpu::Image));
            std::bitset<StageTextureSlots> candidates;
            for (u32 slot = 0; slot < StageTextureSlots; ++slot) {
                candidates[slot] = texture_hash[slot] == hash;
            }
            ++resources.read[Texture];
            resources.found[Texture] += candidates.any();
            Narrow(map.images, k, candidates, map.started);
        }
        for (u32 k = 0; k < p.num_samplers; ++k) {
            const u32 hash = SlotHash(&p.sampler_sharps[k], 16);
            std::bitset<16> candidates;
            for (u32 slot = 0; slot < 16; ++slot) {
                candidates[slot] = sampler_hash[slot] == hash;
            }
            ++resources.read[Sampler];
            resources.found[Sampler] += candidates.any();
            Narrow(map.samplers, k, candidates, map.started);
        }
        ++map.draws;
        if (flat_mode != FlatOff && p.program && p.flat) {
            const auto& buffers = p.program->info.buffers;
            map.buffer_raw.resize(buffers.size());
            map.buffer_raw_known.resize(buffers.size());
            for (u32 k = 0; k < buffers.size() && k < p.num_buffers; ++k) {
                if (map.buffer_raw_known[k]) {
                    continue;
                }
                const auto& fetch = buffers[k].sharp_fetch;
                bool ok = true;
                for (u32 i = 0; i < 4; ++i) {
                    const bool loaded = (fetch.load_mask >> i) & 1;
                    ok = ok && (!loaded || fetch.offsets[i] < p.flat_size);
                    map.buffer_raw[k][i] = loaded && ok ? p.flat[fetch.offsets[i]] : fetch.immediates[i];
                }
                map.buffer_raw_known[k] = ok;
            }
        }
        for (u32 k = 0; k < p.num_buffers; ++k) {
            const AmdGpu::Buffer& sharp = p.buffer_sharps[k];
            std::bitset<14> candidates;
            if (sharp.base_address == 1 && sharp.num_records == 0) {
                candidates.set(); // shadPS4's null buffer, not a resource of the game's
            } else {
                for (u32 slot = 0; slot < 14; ++slot) {
                    candidates[slot] = r.constant_buffer_address[slot] == sharp.base_address &&
                                       (r.constant_buffer_size[slot] + 15) / 16 == sharp.num_records;
                }
                ++resources.read[ConstantBuffer];
                resources.found[ConstantBuffer] += candidates.any();
            }
            Narrow(map.buffers, k, candidates, map.started);
        }
        map.started = true;
        if (take_mesh_draws && d.mesh) {
            TakeResources(p, r, map);
        }
    }
    if (prepared->vertex.valid && vertex_stage) {
        // One V# per attribute: a slot's address plus the attribute's offset, with the slot's stride.
        SlotMap& map = slot_maps[{vertex_stage->program, d.input_layout}];
        for (u32 k = 0; k < prepared->vertex.count; ++k) {
            const AmdGpu::Buffer& sharp = prepared->vertex.buffers[k];
            std::bitset<32> candidates;
            for (u32 slot = 0; slot < 32; ++slot) {
                candidates[slot] = d.vertex_address[slot] && d.vertex_stride[slot] == sharp.stride &&
                                   sharp.base_address - d.vertex_address[slot] < d.vertex_stride[slot];
            }
            ++resources.read[VertexBuffer];
            resources.found[VertexBuffer] += candidates.any();
            Narrow(map.attributes, k, candidates, map.started);
            if (map.attribute_offsets.size() <= k) {
                map.attribute_offsets.resize(k + 1, INT64_MIN);
            }
            if (candidates.any()) {
                const u32 slot = FirstSlot(candidates);
                const s64 offset = s64(sharp.base_address) - s64(d.vertex_address[slot]);
                s64& known = map.attribute_offsets[k];
                known = !map.started ? offset : known == offset ? known : INT64_MIN;
            }
        }
        map.started = true;
        if (take_mesh_draws && d.mesh) {
            TakeVertexBuffers(*prepared, d, map);
        }
    }
}

// How the learnt slot maps stand: per kind, resources with one slot, several, or none.
void ReportSlotMaps() {
    u64 one[Resources] = {}, several[Resources] = {}, none[Resources] = {};
    auto count = [&](Resource kind, const auto& list) {
        for (const auto& bits : list) {
            const std::size_t n = bits.count();
            (n == 1 ? one : n == 0 ? none : several)[kind] += 1;
        }
    };
    for (const auto& [key, map] : slot_maps) {
        count(Texture, map.images);
        count(Sampler, map.samplers);
        count(ConstantBuffer, map.buffers);
        count(VertexBuffer, map.attributes);
    }
    std::printf("Render boundary: slot maps of %zu programs and vertex layouts; one slot/several/none:",
                slot_maps.size());
    for (int k = 0; k < Resources; ++k) {
        std::printf(" %s %llu/%llu/%llu", resource_names[k], static_cast<unsigned long long>(one[k]),
                    static_cast<unsigned long long>(several[k]), static_cast<unsigned long long>(none[k]));
    }
    std::printf("\n");
}

// ---- N2.3: the flattened user data of mesh draws from the draw state ----
// A plan per shader program whose slot map is complete (each texture, sampler and constant buffer
// with at least one candidate slot, the buffers' V# dwords known, or the null buffer), learnt over
// at least PlanDraws draws; the GPU thread publishes them and the workers read a copy. A resource
// with several candidates (slots that held the same sharp in every draw so far, the engine's
// default samplers) is filled only while its candidates still agree: the right slot is always
// among them, so then any gives its sharp.
constexpr u64 PlanDraws = 256;
struct Plan {
    std::vector<std::bitset<StageTextureSlots>> images;
    std::vector<std::bitset<16>> samplers;
    std::vector<std::bitset<14>> buffers; // all bits: the null buffer
    std::vector<std::array<u32, 4>> buffer_raw;
};
using Plans = std::unordered_map<const void*, Plan>;
std::mutex plans_mutex;
std::shared_ptr<const Plans> published_plans;
std::atomic<u64> plans_generation{0};
struct FlatTally {
    std::atomic<u64> stages, filled, no_record, no_plan, failed, sharps, differed, gpu_filled, gpu_failed;
} flat_tally;
// The GPU thread's record of the draw it is drawing (ObserveDraw, flat mode).
Record gpu_record;
bool gpu_has_record = false;

void PublishPlans() {
    auto plans = std::make_shared<Plans>();
    for (const auto& [key, map] : slot_maps) {
        if (key.second != 0 || map.draws < PlanDraws) {
            continue; // vertex attribute maps, or not learnt long enough
        }
        Plan plan;
        bool complete = true;
        for (const auto& bits : map.images) {
            complete = complete && bits.any();
        }
        for (const auto& bits : map.samplers) {
            complete = complete && bits.any();
        }
        for (u32 k = 0; k < map.buffers.size(); ++k) {
            const auto& bits = map.buffers[k];
            const bool known = k < map.buffer_raw_known.size() && map.buffer_raw_known[k];
            complete = complete && (bits.all() || (bits.any() && known));
            plan.buffer_raw.push_back(known ? map.buffer_raw[k] : std::array<u32, 4>{});
        }
        plan.images = map.images;
        plan.samplers = map.samplers;
        plan.buffers = map.buffers;
        if (complete) {
            plans->emplace(key.first, std::move(plan));
        }
    }
    std::scoped_lock lock{plans_mutex};
    published_plans = std::move(plans);
    plans_generation.fetch_add(1, std::memory_order_release);
}

// The worker's view: the tag before the draw it prepares, that draw's record, its copy of the plans.
thread_local u32 worker_tag_number = 0;
thread_local const void* worker_tag_next = nullptr;
thread_local bool worker_has_record = false;
thread_local Record worker_record;
thread_local std::shared_ptr<const Plans> worker_plans;
thread_local u64 worker_plans_generation = 0;

const Plan* WorkerPlan(const void* program) {
    const u64 generation = plans_generation.load(std::memory_order_acquire);
    if (generation != worker_plans_generation) {
        std::scoped_lock lock{plans_mutex};
        worker_plans = published_plans;
        worker_plans_generation = generation;
    }
    if (!worker_plans) {
        return nullptr;
    }
    const auto it = worker_plans->find(program);
    return it == worker_plans->end() ? nullptr : &it->second;
}

// The dwords of a sharp the shader loads from the flattened user data, written where it loads them.
template <typename T, std::size_t N>
bool Place(const Shader::SharpFetch<T>& fetch, const std::array<u32, N>& dwords, std::vector<u32>& flat) {
    for (u32 i = 0; i < N && i < Shader::SharpFetch<T>::N; ++i) {
        if (!((fetch.load_mask >> i) & 1)) {
            continue;
        }
        if (fetch.offsets[i] >= flat.size()) {
            return false;
        }
        flat[fetch.offsets[i]] = dwords[i];
    }
    return true;
}

// The flattened user data of `info`'s stage from the draw state: the user-data registers, then
// each resource's sharp at its offsets. False when a resource has no sharp there.
// The sharp of a resource with candidate slots `bits`, if every candidate holds the same one
// (`read` gives a slot's object address, or 0 for an empty slot).
template <std::size_t Bits, std::size_t Dwords>
bool AgreedSharp(const std::bitset<Bits>& bits, auto read, std::array<u32, Dwords>& out) {
    bool found = false;
    for (u32 slot = 0; slot < Bits; ++slot) {
        if (!bits[slot]) {
            continue;
        }
        const u64 address = read(slot);
        if (!address) {
            return false;
        }
        std::array<u32, Dwords> dwords;
        std::memcpy(dwords.data(), reinterpret_cast<const void*>(address), sizeof(dwords));
        if (!found) {
            out = dwords;
            found = true;
        } else if (dwords != out) {
            return false; // the candidates disagree now: the walker decides
        }
    }
    return found;
}

bool FillFrom(const Plan& plan, const Record::Resources& r, const Shader::Info& info, std::vector<u32>& flat) {
    if (plan.images.size() != info.images.size() || plan.samplers.size() != info.samplers.size() ||
        plan.buffers.size() != info.buffers.size()) {
        return false;
    }
    flat.assign(info.srt_info.flattened_bufsize_dw, 0);
    if (flat.size() < info.user_data.size()) {
        return false;
    }
    std::memcpy(flat.data(), info.user_data.data(), info.user_data.size_bytes());
    for (u32 k = 0; k < info.images.size(); ++k) {
        std::array<u32, 8> dwords;
        const auto texture = [&](u32 slot) { return r.texture[slot] ? r.texture[slot] + TextureSharp : 0; };
        if (!AgreedSharp(plan.images[k], texture, dwords) || !Place(info.images[k].sharp_fetch, dwords, flat)) {
            return false;
        }
    }
    for (u32 k = 0; k < info.samplers.size(); ++k) {
        std::array<u32, 4> dwords;
        const auto sampler = [&](u32 slot) { return r.sampler[slot]; };
        if (!AgreedSharp(plan.samplers[k], sampler, dwords) || !Place(info.samplers[k].sharp_fetch, dwords, flat)) {
            return false;
        }
    }
    for (u32 k = 0; k < info.buffers.size(); ++k) {
        const auto& bits = plan.buffers[k];
        if (info.buffers[k].IsSpecial() || bits.all()) {
            continue; // the renderer's own buffer, or the null buffer: nothing loaded for it
        }
        // Every candidate slot with the same address and size.
        s32 slot = -1;
        for (u32 c = 0; c < 14; ++c) {
            if (!bits[c]) {
                continue;
            }
            if (!r.constant_buffer_address[c]) {
                return false;
            }
            if (slot < 0) {
                slot = s32(c);
            } else if (r.constant_buffer_address[c] != r.constant_buffer_address[slot] ||
                       r.constant_buffer_size[c] != r.constant_buffer_size[slot]) {
                return false;
            }
        }
        if (slot < 0) {
            return false;
        }
        AmdGpu::Buffer sharp;
        std::memcpy(&sharp, plan.buffer_raw[k].data(), sizeof(sharp));
        sharp.base_address = r.constant_buffer_address[slot];
        sharp.num_records = (r.constant_buffer_size[slot] + 15) / 16;
        std::array<u32, 4> dwords;
        std::memcpy(dwords.data(), &sharp, sizeof(dwords));
        if (!Place(info.buffers[k].sharp_fetch, dwords, flat)) {
            return false;
        }
    }
    return true;
}

// The record's stage of a mesh draw's shader stage: the vertex and pixel shaders (mesh draws are
// neither tessellated nor geometry-shaded); -1 for any other.
int MeshStage(Shader::HwStage stage) {
    return stage == Shader::HwStage::Vertex ? int(Vertex) : stage == Shader::HwStage::Fragment ? int(Pixel) : -1;
}

void Report(u64 now) {
    if (flat_mode != FlatOff) {
        PublishPlans();
        std::printf("Render boundary: flat (%s): stages %llu, from the draw state %llu, without a record %llu, "
                    "without a plan %llu, a resource without a sharp %llu; sharps compared %llu, differing %llu; "
                    "plans %zu; on the GPU thread from the draw state %llu, not %llu\n",
                    flat_mode == FlatFill ? "fill" : "check",
                    static_cast<unsigned long long>(flat_tally.stages.load()),
                    static_cast<unsigned long long>(flat_tally.filled.load()),
                    static_cast<unsigned long long>(flat_tally.no_record.load()),
                    static_cast<unsigned long long>(flat_tally.no_plan.load()),
                    static_cast<unsigned long long>(flat_tally.failed.load()),
                    static_cast<unsigned long long>(flat_tally.sharps.load()),
                    static_cast<unsigned long long>(flat_tally.differed.load()),
                    published_plans ? published_plans->size() : std::size_t(0),
                    static_cast<unsigned long long>(flat_tally.gpu_filled.load()),
                    static_cast<unsigned long long>(flat_tally.gpu_failed.load()));
    }
    std::printf("Render boundary: method calls");
    for (u32 m = 0; m < Methods; ++m) {
        std::printf(" %s %llu", method_names[m], static_cast<unsigned long long>(method_calls[m].load()));
    }
    std::printf("; tagged %llu, no room to tag %llu, libGnm draws outside %llu, inside a method but not its "
                "own %llu\n",
                static_cast<unsigned long long>(filed.load()), static_cast<unsigned long long>(no_room.load()),
                static_cast<unsigned long long>(gnm_without_method.load()),
                static_cast<unsigned long long>(gnm_nested.load()));
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
    std::printf("\nRender boundary: resources: draws prepared %llu, not %llu; found/read:",
                static_cast<unsigned long long>(resources.prepared), static_cast<unsigned long long>(resources.unprepared));
    for (int k = 0; k < Resources; ++k) {
        std::printf(" %s %llu/%llu", resource_names[k], static_cast<unsigned long long>(resources.found[k]),
                    static_cast<unsigned long long>(resources.read[k]));
    }
    std::printf("\n");
    ReportSlotMaps();
    ReportInventory();
    if (take_mesh_draws) {
        std::printf("Render boundary: mesh draws from the draw state %llu, resources taken %llu, differing from the "
                    "user data's %llu\n",
                    static_cast<unsigned long long>(taken.draws), static_cast<unsigned long long>(taken.resources),
                    static_cast<unsigned long long>(taken.differed));
    }
    std::fflush(stdout);
    last_report = now;
}
} // namespace

bool Observing() {
    return observing;
}

bool FlatModeOn() {
    return flat_mode != FlatOff;
}

void WorkerNoteTag(const void* nop) {
    worker_tag_number = static_cast<const u32*>(nop)[2];
    worker_tag_next = static_cast<const u32*>(nop) + 3;
}

void WorkerDraw(const void* packet) {
    worker_has_record = packet == worker_tag_next && Find(worker_tag_number, worker_record) && worker_record.mesh;
}

bool WorkerFlat(const void* program, Shader::Info& info, Shader::HwStage stage) {
    if (flat_mode != FlatFill) {
        return false;
    }
    flat_tally.stages.fetch_add(1, std::memory_order_relaxed);
    const int s = MeshStage(stage);
    if (!worker_has_record || s < 0) {
        flat_tally.no_record.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const Plan* plan = WorkerPlan(program);
    if (!plan) {
        flat_tally.no_plan.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!FillFrom(*plan, worker_record.stage[s], info, info.flattened_ud_buf)) {
        flat_tally.failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    flat_tally.filled.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool GpuFlat(const void* program, Shader::Info& info, Shader::HwStage stage) {
    const int s = MeshStage(stage);
    const Plan* plan = s >= 0 && gpu_has_record ? WorkerPlan(program) : nullptr;
    if (!plan || !FillFrom(*plan, gpu_record.stage[s], info, info.flattened_ud_buf)) {
        flat_tally.gpu_failed.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    flat_tally.gpu_filled.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void WorkerCheckFlat(const void* program, Shader::Info& info, Shader::HwStage stage) {
    if (flat_mode != FlatCheck) {
        return;
    }
    flat_tally.stages.fetch_add(1, std::memory_order_relaxed);
    const int s = MeshStage(stage);
    if (!worker_has_record || s < 0) {
        flat_tally.no_record.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const Plan* plan = WorkerPlan(program);
    if (!plan) {
        flat_tally.no_plan.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    thread_local std::vector<u32> filled;
    if (!FillFrom(*plan, worker_record.stage[s], info, filled)) {
        flat_tally.failed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    flat_tally.filled.fetch_add(1, std::memory_order_relaxed);
    // The sharps each buffer gives, as the shader and PrepareResources read them.
    u64 compared = 0, differed = 0;
    auto check = [&](const auto& list) {
        for (const auto& res : list) {
            const auto walked = res.GetSharp(info);
            std::swap(info.flattened_ud_buf, filled);
            const auto from_state = res.GetSharp(info);
            std::swap(info.flattened_ud_buf, filled);
            ++compared;
            differed += std::memcmp(&walked, &from_state, sizeof(walked)) != 0;
        }
    };
    check(info.images);
    check(info.samplers);
    check(info.buffers);
    flat_tally.sharps.fetch_add(compared, std::memory_order_relaxed);
    flat_tally.differed.fetch_add(differed, std::memory_order_relaxed);
}

void NoteTag(const void* nop) {
    tag_number = static_cast<const u32*>(nop)[2];
    tag_next = static_cast<const u32*>(nop) + 3;
}

void ObserveDraw(const void* packet, const AmdGpu::Regs& regs, bool indexed, u32 index_offset, bool indirect,
                 const Vulkan::PreparedDraw* prepared, u64 reg_checksum) {
    Record d;
    const bool found = packet == tag_next && Find(tag_number, d);
    if (flat_mode == FlatFill) {
        gpu_has_record = found && d.mesh;
        if (gpu_has_record) {
            gpu_record = d;
        }
    }
    if (!found) {
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
        CompareOutputs(d, regs);
        CompareResources(d, regs, prepared, reg_checksum);
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
                if (shader_examples_left > 0) {
                    --shader_examples_left;
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
    if (!env || (std::strcmp(env, "observe") != 0 && std::strcmp(env, "mesh") != 0 &&
                 std::strcmp(env, "flat-check") != 0 && std::strcmp(env, "flat") != 0)) {
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
    take_mesh_draws = observing && std::strcmp(env, "mesh") == 0;
    flat_mode = !observing ? FlatOff : std::strcmp(env, "flat") == 0 ? FlatFill
              : std::strcmp(env, "flat-check") == 0 ? FlatCheck : FlatOff;
    std::printf("Render boundary: observing draws%s (%d of %d hooks)\n",
                take_mesh_draws ? ", mesh draws' resources from the draw state" : "", installed, wanted);
    last_report = NowMs();
}

} // namespace BbRenderBoundary
