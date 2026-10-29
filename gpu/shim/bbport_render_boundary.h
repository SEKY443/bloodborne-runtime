// SPDX-License-Identifier: GPL-2.0-or-later
// bloodborne-runtime: the engine's render boundary (N6, docs/NATIVE_RENDERER.md). For now it only
// observes (BB_RENDER_BOUNDARY=observe): CoreGraphics2's draw methods record the draw state they
// are given and tag the draw's packet with a NOP naming the record; the packet reader compares the
// record with the registers it decoded. Nothing is drawn differently.
#pragma once

#include <cstdint>

namespace AmdGpu {
union Regs;
}
namespace Vulkan {
struct PreparedDraw;
}
namespace Shader {
struct Info;
enum class HwStage : std::uint32_t;
}

namespace BbRenderBoundary {

/// Installs the hooks when BB_RENDER_BOUNDARY=observe (from bbgpu_patch_image). Not with the draw
/// census, which hooks the same functions.
void PatchImage(unsigned char* image, std::uint64_t size);

/// Whether draws are observed (the packet reader's test before ObserveDraw).
bool Observing();

/// The first payload dword of the NOP that tags a draw (a value libGnm's own NOPs do not use).
constexpr std::uint32_t TagPayload = 0x424e3600;

/// The packet reader, at a NOP whose payload starts with TagPayload: the draw after it is tagged.
void NoteTag(const void* nop);

/// The packet reader, at a draw packet in the game's own command buffer, with the registers it
/// draws with. `indexed`: DrawIndex2 or DrawIndexOffset2; `index_offset`: DrawIndexOffset2's first
/// index (0 otherwise); `indirect`: the counts are in memory. `prepared`: the draw-preparation
/// worker's resources for the draw (null if none), valid when ready and its register checksum is
/// `reg_checksum`.
void ObserveDraw(const void* packet, const AmdGpu::Regs& regs, bool indexed, std::uint32_t index_offset,
                 bool indirect, const Vulkan::PreparedDraw* prepared, std::uint64_t reg_checksum);

/// N2.3 (BB_RENDER_BOUNDARY=flat or flat-check): whether the draw-preparation workers read the
/// boundary's records.
bool FlatModeOn();
/// A draw-preparation worker, at a NOP tag and then at each draw packet it prepares.
void WorkerNoteTag(const void* nop);
void WorkerDraw(const void* packet);
/// flat: the stage's flattened user data written from the draw state, without the walker; false
/// when the draw has no record, its program no complete slot map, or a resource no sharp there.
bool WorkerFlat(const void* program, Shader::Info& info, Shader::HwStage stage);
/// flat-check, after the walker: fills a second buffer from the draw state and compares the
/// sharps each gives.
void WorkerCheckFlat(const void* program, Shader::Info& info, Shader::HwStage stage);
/// flat, on the GPU thread checking a prepared stage the worker filled: the same fill from the
/// draw's record (kept by ObserveDraw), into `info`'s flattened user data; false without one.
bool GpuFlat(const void* program, Shader::Info& info, Shader::HwStage stage);

/// N2.4, compute passes. The packet reader, at a dispatch packet in the game's own command buffer:
/// a tag before it names the record of its compute state (CG::ResourceBinder::commitCompute).
void ObserveDispatch(const void* packet);
/// flat, on the GPU thread selecting the dispatch's program: its flattened user data written from
/// the compute state, without the walker; false without a record or a complete plan.
bool GpuComputeFlat(const void* program, Shader::Info& info, Shader::HwStage stage);
/// After the walker: the program's slot map learnt from the dispatch (until it has a plan), and
/// with flat-check the fill compared.
void LearnCompute(const void* program, Shader::Info& info, Shader::HwStage stage);

} // namespace BbRenderBoundary
