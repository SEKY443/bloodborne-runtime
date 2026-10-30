// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/renderer_backend/transfers.h"
#include "common/enum.h"
#include "common/incremental_id.h"
#include "common/types.h"
#include "video_core/renderer_backend/device.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/image_upload_hash.h"
#include "video_core/texture_cache/image_view.h"

#include <deque>
#include <optional>
#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

namespace Vulkan {
class Instance;
class Runtime;
} // namespace Vulkan

namespace VideoCore {

enum ImageFlagBits : u32 {
    Empty = 0,
    MaybeCpuDirty = 1 << 0, ///< The page this image is in was touched before the image address
    CpuDirty = 1 << 1,      ///< Contents have been modified from the CPU
    GpuDirty = 1 << 2, ///< Contents have been modified from the GPU (valid data in buffer cache)
    Dirty = MaybeCpuDirty | CpuDirty | GpuDirty,
    GpuModified = 1 << 3, ///< Contents have been modified from the GPU
    Registered = 1 << 6,  ///< True when the image is registered
    Picked = 1 << 7,      ///< Temporary flag to mark the image as picked
};
DECLARE_ENUM_FLAG_OPERATORS(ImageFlagBits)

/// bbport: lazy texture usages (Image::allowed_storage); BB_LAZY_TEXTURE_USAGE=0 turns them off.
bool LazyTextureUsage();

struct Image {
    explicit Image(Backend::Device& device,
                   VideoCore::Backend::Transfers& runtime, Common::SlotVector<ImageView>& slot_image_views,
                   const ImageInfo& info);
    ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    Image(Image&&) = default;
    Image& operator=(Image&&) = default;

    bool Overlaps(VAddr overlap_cpu_addr, size_t overlap_size) const noexcept {
        const VAddr overlap_end = overlap_cpu_addr + overlap_size;
        const auto image_addr = info.guest_address;
        const auto image_end = info.guest_address + info.guest_size;
        return image_addr < overlap_end && overlap_cpu_addr < image_end;
    }

    /// The backend's image of the current backing (for Vulkan code until it records through
    /// the adapter).
    vk::Image GetImage() const {
        return vk::Image{reinterpret_cast<VkImage>(backing->native)};
    }

    vk::DeviceSize GetHostImageSize() const {
        return backing->size_bytes;
    }

    bool IsTracked() {
        return track_addr != 0 && track_addr_end != 0;
    }

    bool SafeToDownload() const {
        return True(flags & ImageFlagBits::GpuModified) && False(flags & (ImageFlagBits::Dirty));
    }

    void AssociateDepth(ImageId depth_image_id, u64 depth_image_uid) {
        depth_id = depth_image_id;
        depth_uid = depth_image_uid;
    }

    void DisassociateDepth() {
        depth_id = {};
        depth_uid = {};
    }

    ImageView& FindView(const ImageViewInfo& view_info, bool ensure_guest_samples = true);


public:
    struct BackingImage;
    // bbport: fields every binding of the image reads, together ahead of the large ImageInfo
    // (they were spread over several cache lines: one miss each per texture per draw).
    ImageFlagBits flags = ImageFlagBits::Dirty;
    struct {
        u32 is_bound : 1;
        u32 is_target : 1;
        u32 needs_rebind : 1;
        u32 force_general : 1;
    } binding{};
    struct {
        u32 texture : 1;
        u32 storage : 1;
        u32 render_target : 1;
        u32 depth_target : 1;
        u32 vo_surface : 1;
    } usage{};
    bool scene_proxy = false; ///< bbport: SceneTargets holds a reduced-size proxy of it
    /// bbport (BB_DEFER_CLEARS): a clear not recorded yet (Runtime::ClearImage). The next render
    /// pass that draws into exactly this range clears it on load; anything else that uses the
    /// image records it first (Runtime::ResolvePendingClear).
    bool pending_clear = false;
    SubresourceRange pending_clear_range{};
    Backend::ClearColor pending_clear_value{};
    VAddr track_addr = 0;
    VAddr track_addr_end = 0;
    VAddr guest_begin = 0; ///< info.guest_address
    VAddr guest_end = 0;   ///< info.guest_address + info.guest_size
    BackingImage* backing{};
    /// bbport: gc tick of the last LRU touch; skips the LRU list (a cache miss) when current.
    mutable u64 lru_touched_tick = ~0ULL;
    u64 tick_accessed_last{};
    /// bbport: scheduler tick of the last binding that lets the GPU write it (render or depth target,
    /// storage image); 0 never.
    u64 gpu_write_tick{};
    ImageId depth_id{};
    u64 depth_uid{};
    u64 image_uid{};
    u8 prompt_icon{};  ///< bbport 0.6: the game's key-guide icon identified from its own bytes
    u8 prompt_atlas{}; ///< HUD atlas containing key-guide icons
    u32 prompt_upload{};

    ImageInfo info;
    Backend::Device* device{};
    VideoCore::Backend::Transfers* runtime;
    Common::SlotVector<ImageView>* slot_image_views;
    vk::ImageAspectFlags aspect_mask = vk::ImageAspectFlagBits::eColor;
    vk::SampleCountFlags supported_samples = vk::SampleCountFlagBits::e1;

    vk::ImageUsageFlags usage_flags;
    vk::FormatFeatureFlags2 format_features;
    /// bbport: the storage and format-view usages the image may need (after Mojahed Abushawish's
    /// lazy usages for bloodborne_mac_port, #33). Apple GPUs compress a texture losslessly only
    /// without them, so single-sample colour and depth-only backings are made without them and
    /// get both, once, when a storage view or a view in another pixel format needs one
    /// (NeededUsage, Transfers::UpgradeUsage).
    bool allowed_storage = false;
    bool allowed_format_views = false;
    /// `desc` with every usage the image may need.
    void AllowedUsage(Backend::ImageDesc& desc) const;
    /// Whether a view `view_info` of the current backing needs a usage the backing lacks.
    [[nodiscard]] bool NeededUsage(const ImageViewInfo& view_info) const;
    /// A new image (no contents, no views yet) a binding needs every usage for: its backing made
    /// again with them, without the copy an upgrade makes.
    void ApplyNewUsage();
    /// Whether the current backing has every usage the image may need (upgraded, or never lazy).
    [[nodiscard]] bool HasAllowedUsage() const;
    /// One allocation of the image (another sample count gets another backing), created through
    /// the render adapter (docs/RENDER_ADAPTER.md, A2).
    struct BackingImage {
        Backend::ImageHandle handle;
        u64 native{};
        Backend::ImageDesc desc; ///< as created: the supported format and sample count
        u64 size_bytes{};
        /// The backend's state of this allocation (Backend::ImageAllocation::backend_state):
        /// the Vulkan backend's layouts and last accesses (vk_resource_backend.h).
        void* backend_state{};
        boost::container::small_vector<ImageViewInfo, 4> image_view_infos;
        boost::container::small_vector<ImageViewId, 4> image_view_ids;
        u32 last_view = 0; ///< bbport: index of the view FindView returned last
        u32 num_samples;
    };
    std::deque<BackingImage> backing_images;
    boost::container::static_vector<ImageUploadHash, 16> mip_hashes{};
    u64 lru_id{};
    u64 hash{};

private:
    static Common::IncrementalIdProvider<u64> global_image_uid;
};

} // namespace VideoCore
