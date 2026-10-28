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

} // namespace BbRenderBoundary
