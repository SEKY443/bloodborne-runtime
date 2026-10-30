// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <set>
#include <mutex>
#include "bbport_toggles.h"
#include <ranges>
#include "bbport_metal_residency.h"
#include "common/assert.h"
#include <string>
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_bind_helper.h"
#include "video_core/renderer_vulkan/vk_resource_backend.h"
#include "video_core/renderer_vulkan/vk_state.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

bool LazyTextureUsage() {
    static const bool on = [] {
        const char* env = std::getenv("BB_LAZY_TEXTURE_USAGE");
        return !(env && env[0] == '0');
    }();
    return on;
}

namespace {
constexpr u32 StorageBit = static_cast<u32>(Backend::ImageUse::Storage);

bool HasStorage(const Backend::ImageDesc& desc) {
    return (static_cast<u32>(desc.usage) & StorageBit) != 0;
}

// An sRGB format and its linear twin: Metal views one as the other without PixelFormatView.
bool SrgbTwins(Backend::Format a, Backend::Format b) {
    const auto linear = [](Backend::Format format) {
        std::string name = Backend::FormatName(format);
        if (const auto at = name.find("Srgb"); at != std::string::npos) {
            name.replace(at, 4, "Unorm");
        }
        return name;
    };
    return linear(a) == linear(b);
}
} // namespace

static Backend::ImageUse ImageUsage(const Backend::Capabilities& caps, const ImageInfo& info) {
    using Backend::ImageUse;
    ImageUse usage = ImageUse::TransferSource | ImageUse::TransferDestination | ImageUse::Sampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage = usage | ImageUse::DepthStencilAttachment;
        } else {
            usage = usage | ImageUse::ColorAttachment;
            if (caps.attachment_feedback_loop) {
                usage = usage | ImageUse::AttachmentFeedbackLoop;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used.
            usage = usage | ImageUse::Storage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage = usage | ImageUse::Storage;
    }

    return usage;
}

static Backend::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
        return Backend::ImageType::Image1D;
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
        return Backend::ImageType::Image2D;
    case AmdGpu::ImageType::Color3D:
        return Backend::ImageType::Image3D;
    default:
        UNREACHABLE();
    }
}


Image::Image(Backend::Device& device_,
             VideoCore::Backend::Transfers& runtime_, Common::SlotVector<ImageView>& slot_image_views_,
             const ImageInfo& info_)
    : guest_begin{info_.guest_address}, guest_end{info_.guest_address + info_.guest_size},
      info{info_}, device{&device_}, runtime{&runtime_}, slot_image_views{&slot_image_views_} {
    BbStats::Timer timer{BbStats::t_image_create};
    if (info.pixel_format == Backend::Format::Undefined) {
        return;
    }

    image_uid = global_image_uid.Next();
    mip_hashes.resize(info.resources.levels);

    const Backend::ImageUse usage = ImageUsage(device->GetCapabilities(), info);
    usage_flags = ToVk(usage);
    format_features = Vulkan::FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    Backend::ImageDesc desc{
        .type = ConvertImageType(info.type),
        .format = device->SupportedFormat(info.pixel_format, usage),
        .width = info.size.width,
        .height = info.size.height,
        .depth = info.size.depth,
        .levels = static_cast<u32>(info.resources.levels),
        .layers = static_cast<u32>(info.resources.layers),
        .usage = usage,
        .views_of_3d_slices = bool(info.props.is_volume),
        .uncompressed_block_views = bool(info.props.is_block),
    };
    supported_samples = vk::SampleCountFlags(device->SupportedSampleCounts(desc));
    desc.samples = static_cast<u32>(LiverpoolToVK::NumSamples(info.num_samples, supported_samples));

    // bbport: lazy usages (allowed_storage): a stencil plane is not copied by an upgrade, so
    // images with one keep every usage from the start, as multisample and block images do.
    allowed_storage = HasStorage(desc);
    allowed_format_views = desc.mutable_format;
    if (LazyTextureUsage() && !info.props.is_block && !info.props.has_stencil && desc.samples == 1) {
        desc.usage = static_cast<Backend::ImageUse>(static_cast<u32>(desc.usage) & ~StorageBit);
        desc.mutable_format = false;
    }

    backing = &backing_images.emplace_back();
    backing->num_samples = info.num_samples;
    backing->desc = desc;
    const auto allocation = device->CreateImage(
        desc, fmt::format("Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          Backend::FormatName(info.pixel_format), info.guest_address,
                          info.guest_size, info.resources.layers, info.resources.levels,
                          info.num_samples));
    backing->handle = allocation.handle;
    backing->native = allocation.native;
    backing->size_bytes = allocation.size_bytes;
    backing->backend_state = allocation.backend_state;
}

void Image::AllowedUsage(Backend::ImageDesc& desc) const {
    if (allowed_storage) {
        desc.usage = static_cast<Backend::ImageUse>(static_cast<u32>(desc.usage) | StorageBit);
    }
    desc.mutable_format = allowed_format_views;
}

bool Image::NeededUsage(const ImageViewInfo& view_info) const {
    const Backend::ImageDesc& desc = backing->desc;
    if (desc.samples > 1) {
        return false;
    }
    if (view_info.is_storage && allowed_storage && !HasStorage(desc)) {
        return true;
    }
    if (!allowed_format_views || desc.mutable_format) {
        return false;
    }
    // The format ImageView makes the view in: a depth image's own for depth views.
    if (info.props.is_depth && Vulkan::LiverpoolToVK::IsFormatDepthCompatible(view_info.format)) {
        return false;
    }
    const Backend::Format format = device->SupportedFormat(view_info.format, Vulkan::FromVk(usage_flags));
    return format != desc.format && !SrgbTwins(format, desc.format);
}

bool Image::HasAllowedUsage() const {
    if (!backing) {
        return true;
    }
    Backend::ImageDesc desc = backing->desc;
    AllowedUsage(desc);
    return desc == backing->desc;
}

void Image::ApplyNewUsage() {
    if (!backing || backing_images.size() != 1 || !backing->image_view_ids.empty()) {
        return;
    }
    Backend::ImageDesc desc = backing->desc;
    AllowedUsage(desc);
    if (desc == backing->desc) {
        return;
    }
    device->DestroyImage(backing->handle); // never used yet
    const auto allocation = device->CreateImage(
        desc, fmt::format("Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width, info.size.height,
                          info.size.depth, AmdGpu::NameOf(info.tile_mode), Backend::FormatName(info.pixel_format),
                          info.guest_address, info.guest_size, info.resources.layers, info.resources.levels,
                          info.num_samples));
    backing->desc = desc;
    backing->handle = allocation.handle;
    backing->native = allocation.native;
    backing->size_bytes = allocation.size_bytes;
    backing->backend_state = allocation.backend_state;
}

Image::~Image() {
    // Stale handles (a moved-from image's) are ignored by the device.
    for (const BackingImage& b : backing_images) {
        if (b.handle) {
            device->DestroyImage(b.handle);
        }
    }
}


ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        runtime->SetBackingSamples(this, info.num_samples);
    }
    const auto& view_infos = backing->image_view_infos;
    if (backing->last_view < view_infos.size() && view_infos[backing->last_view] == view_info) {
        return (*slot_image_views)[backing->image_view_ids[backing->last_view]];
    }
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        backing->last_view = static_cast<u32>(std::distance(view_infos.begin(), it));
        const auto view_id = backing->image_view_ids[backing->last_view];
        return (*slot_image_views)[view_id];
    }
    // bbport: a view the backing lacks a usage for: the image gets every usage once (a copy).
    // The texture binding helper never upgrades (Rasterizer::TexturesBindableOnHelper).
    if (NeededUsage(view_info)) {
        if (!Vulkan::BindHelper::OnHelper()) {
            runtime->UpgradeUsage(this);
            return FindView(view_info, false);
        }
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            LOG_ERROR(Render_Vulkan, "texture usage upgrade needed on the binding helper");
        }
    }
    const auto view_id = slot_image_views->insert(*device, view_info, *this);
    backing->image_view_infos.emplace_back(view_info);
    backing->image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}


} // namespace VideoCore
