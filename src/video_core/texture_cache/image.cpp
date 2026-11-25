// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/debug_state.h"
#include "video_core/readback_trace.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

using namespace Vulkan;

Common::IncrementalIdProvider<u64> Image::global_image_uid{};

static vk::ImageUsageFlags ImageUsageFlags(const Vulkan::Instance& instance,
                                           const ImageInfo& info) {
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransferSrc |
                                vk::ImageUsageFlagBits::eTransferDst |
                                vk::ImageUsageFlagBits::eSampled;
    if (!info.props.is_block) {
        if (info.props.is_depth) {
            usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        } else {
            usage |= vk::ImageUsageFlagBits::eColorAttachment;
            if (instance.IsAttachmentFeedbackLoopLayoutSupported()) {
                usage |= vk::ImageUsageFlagBits::eAttachmentFeedbackLoopEXT;
            }
            // Always create images with storage flag to avoid needing re-creation in case of e.g
            // compute clears This sacrifices a bit of performance but is less work. ExtendedUsage
            // flag is also used. The exception here is for multisample images when storage is not
            // supported, where even with ExtendedUsage we may get only one supported sample back.
            if (info.num_samples == 1 || instance.IsMultisampleStorageImageSupported())
                usage |= vk::ImageUsageFlagBits::eStorage;
        }
    } else {
        // Similarly to above, we specify storage usage. This is typically not supported by
        // compressed formats, but may be used for uncompressed views. In order to satisfy this,
        // we will also specify the extended usage bit.
        usage |= vk::ImageUsageFlagBits::eStorage;
    }

    return usage;
}

static vk::ImageType ConvertImageType(AmdGpu::ImageType type) noexcept {
    switch (type) {
    case AmdGpu::ImageType::Color1D:
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Cube:
        return vk::ImageType::e2D;
    case AmdGpu::ImageType::Color3D:
        return vk::ImageType::e3D;
    default:
        UNREACHABLE();
    }
}

static vk::FormatFeatureFlags2 FormatFeatureFlags(const vk::ImageUsageFlags usage_flags) {
    vk::FormatFeatureFlags2 feature_flags{};
    if (usage_flags & vk::ImageUsageFlagBits::eTransferSrc) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferSrc;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eTransferDst) {
        feature_flags |= vk::FormatFeatureFlagBits2::eTransferDst;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eSampled) {
        feature_flags |= vk::FormatFeatureFlagBits2::eSampledImage;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eColorAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eColorAttachment;
    }
    if (usage_flags & vk::ImageUsageFlagBits::eDepthStencilAttachment) {
        feature_flags |= vk::FormatFeatureFlagBits2::eDepthStencilAttachment;
    }
    // Note: StorageImage is intentionally ignored for now since it is always set, and can mess up
    // compatibility checks.
    return feature_flags;
}

UniqueImage::~UniqueImage() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
    }
}

void UniqueImage::Destroy() {
    if (image) {
        vmaDestroyImage(allocator, image, allocation);
        image = vk::Image{};
        allocation = {};
    }
}

void UniqueImage::Create(const vk::ImageCreateInfo& image_ci) {
    this->image_ci = image_ci;
    ASSERT(!image);
    VmaAllocationCreateInfo alloc_ci = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    const VkImageCreateInfo image_ci_unsafe = static_cast<VkImageCreateInfo>(image_ci);
    VkImage unsafe_image{};
    VmaAllocationInfo alloc_info{};
    VkResult result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image,
                                     &allocation, &alloc_info);
    if (result == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        alloc_ci.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        LOG_WARNING(Render_Vulkan, "Allocating image of size {} bytes over budget.",
                    alloc_info.size);
        result = vmaCreateImage(allocator, &image_ci_unsafe, &alloc_ci, &unsafe_image, &allocation,
                                &alloc_info);
    }
    ASSERT_MSG(result == VK_SUCCESS, "Failed allocating image with error {}",
               vk::to_string(vk::Result{result}));
    image = vk::Image{unsafe_image};
    size_bytes = alloc_info.size;
}

Image::Image(const Vulkan::Instance& instance, Vulkan::Runtime& runtime_,
             Common::SlotVector<ImageView>& slot_image_views_, const ImageInfo& info_)
    : runtime{&runtime_}, slot_image_views{&slot_image_views_}, info{info_} {
    if (info.pixel_format == vk::Format::eUndefined) {
        return;
    }

    image_uid = global_image_uid.Next();
    vk::ImageCreateFlags flags{vk::ImageCreateFlagBits::eMutableFormat |
                               vk::ImageCreateFlagBits::eExtendedUsage};
    if (info.props.is_volume) {
        flags |= vk::ImageCreateFlagBits::e2DArrayCompatible;
        if (instance.Is2dViewOf3dSupported()) {
            flags |= vk::ImageCreateFlagBits::e2DViewCompatibleEXT;
        }
    }
    if (info.props.is_block && instance.IsBlockTexelViewSupported()) {
        flags |= vk::ImageCreateFlagBits::eBlockTexelViewCompatible;
    }

    usage_flags = ImageUsageFlags(instance, info);
    format_features = FormatFeatureFlags(usage_flags);
    if (info.props.is_depth) {
        aspect_mask = vk::ImageAspectFlagBits::eDepth;
        if (info.props.has_stencil) {
            aspect_mask |= vk::ImageAspectFlagBits::eStencil;
        }
    }

    constexpr auto tiling = vk::ImageTiling::eOptimal;
    const auto supported_format = instance.GetSupportedFormat(info.pixel_format, format_features);
    const vk::PhysicalDeviceImageFormatInfo2 format_info{
        .format = supported_format,
        .type = ConvertImageType(info.type),
        .tiling = tiling,
        .usage = usage_flags,
        .flags = flags,
    };
    const auto image_format_properties =
        instance.GetPhysicalDevice().getImageFormatProperties2(format_info);
    if (image_format_properties.result == vk::Result::eErrorFormatNotSupported) {
        LOG_ERROR(Render_Vulkan, "image format {} type {} is not supported (flags {}, usage {})",
                  vk::to_string(supported_format), vk::to_string(format_info.type),
                  vk::to_string(format_info.flags), vk::to_string(format_info.usage));
    }
    supported_samples = image_format_properties.result == vk::Result::eSuccess
                            ? image_format_properties.value.imageFormatProperties.sampleCounts
                            : vk::SampleCountFlagBits::e1;

    const vk::ImageCreateInfo image_ci = {
        .flags = flags,
        .imageType = ConvertImageType(info.type),
        .format = supported_format,
        .extent{
            .width = info.size.width,
            .height = info.size.height,
            .depth = info.size.depth,
        },
        .mipLevels = static_cast<u32>(info.resources.levels),
        .arrayLayers = static_cast<u32>(info.resources.layers),
        .samples = LiverpoolToVK::NumSamples(info.num_samples, supported_samples),
        .tiling = tiling,
        .usage = usage_flags,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    backing = &AddBacking(image_ci, info.num_samples);

    Vulkan::SetObjectName(instance.GetDevice(), GetImage(),
                          "Image {}x{}x{} {} {} {:#x}:{:#x} L:{} M:{} S:{}", info.size.width,
                          info.size.height, info.size.depth, AmdGpu::NameOf(info.tile_mode),
                          vk::to_string(info.pixel_format), info.guest_address, info.guest_size,
                          info.resources.layers, info.resources.levels, info.num_samples);
}

Image::~Image() = default;

Image::BackingImage& Image::AddBacking(const vk::ImageCreateInfo& create_info, u32 num_samples) {
    const auto previous_size = memory_usage_counter ? Common::AlignUp(SizeBytes(), 1024) : 0;
    const size_t backing_index = backing ? backing - backing_images.data() : 0;
    auto& added_backing = backing_images.emplace_back();
    backing = &backing_images[backing_index];

    const auto& instance = runtime->GetInstance();
    added_backing.num_samples = num_samples;
    added_backing.image = UniqueImage{instance.GetDevice(), instance.GetAllocator()};
    added_backing.image.Create(create_info);
    if (memory_usage_counter) {
        *memory_usage_counter += Common::AlignUp(SizeBytes(), 1024) - previous_size;
    }
    return added_backing;
}

ImageView& Image::FindView(const ImageViewInfo& view_info, bool ensure_guest_samples) {
    if (ensure_guest_samples && backing->num_samples > 1 != info.num_samples > 1) {
        runtime->SetBackingSamples(this, info.num_samples);
    }
    return FindView(view_info, *backing);
}

ImageView& Image::FindView(const ImageViewInfo& view_info, BackingImage& view_backing) {
    const auto& view_infos = view_backing.image_view_infos;
    const auto it = std::ranges::find(view_infos, view_info);
    if (it != view_infos.end()) {
        const auto view_id = view_backing.image_view_ids[std::distance(view_infos.begin(), it)];
        return (*slot_image_views)[view_id];
    }
    const auto view_id = slot_image_views->Insert(runtime->GetInstance(), view_info, *this,
                                                  view_backing.image.image);
    view_backing.image_view_infos.emplace_back(view_info);
    view_backing.image_view_ids.emplace_back(view_id);
    return (*slot_image_views)[view_id];
}

namespace {

/// SHADPS4_TRACE_BARRIERS statistics (local investigation aid).
struct BarrierStats {
    struct PerImage {
        u64 calls{};
        u64 hits{};
        u64 visited{};
        u32 levels{};
        u32 layers{};
        SubresourceRange range{};
        u32 width{};
        u32 height{};
        u32 depth{};
        u32 type{};
    };
    // Hot counters are relaxed atomics; the mutex only guards the per-image map (filled on the
    // rare full partial walks) and the periodic dump, so tracing barely slows the GPU thread.
    std::mutex mutex;
    std::atomic<u64> full_calls{};
    std::atomic<u64> partial_calls{};
    std::atomic<u64> memo_hits{};
    std::atomic<u64> visited{};
    std::atomic<u64> barriers{};
    std::atomic<u64> ticks{};
    std::unordered_map<const void*, PerImage> images;
    std::chrono::steady_clock::time_point last_dump = std::chrono::steady_clock::now();
    u32 last_flips = DebugState.GetFrameNum();
    std::string opts_at_start = ActiveOpts();

    static std::string ActiveOpts() {
        using namespace VideoCore::ReadbackTrace;
        std::string s;
        const auto add = [&s](bool on, const char* name) {
            if (on) {
                s += s.empty() ? name : std::string{"+"} + name;
            }
        };
        add(BarrierMemo(), "memo");
        add(FindImageCache(), "find");
        add(FastAccessLists(), "tree");
        add(LazyReadRanges(), "lazy");
        add(ImageDescCache(), "desc");
        add(StreamDedupe(), "dedupe");
        add(DescSlotReuse(), "slot");
        add(PerfSwitchActive(EnvNumber("SHADPS4_COPY_WORKER", 0)), "worker");
        return s.empty() ? "none" : s;
    }

    /// Cheap per-call hook: only every 1024th call looks at the clock.
    void MaybeDump() {
        const u64 tick = ticks.load(std::memory_order_relaxed);
        ticks.store(tick + 1, std::memory_order_relaxed);
        if ((tick & 1023) == 0) {
            std::scoped_lock lk{mutex};
            Dump();
        }
    }

    /// "Top opcodes" list of one PM4 queue, e.g. "0x2d=1234 0x69=987", counts per frame.
    static std::string TopOps(std::array<std::atomic<u64>, 256>& ops, u32 frames, size_t top) {
        std::vector<std::pair<u64, u32>> list;
        for (u32 op = 0; op < ops.size(); ++op) {
            if (const u64 n = ops[op].exchange(0)) {
                list.emplace_back(n, op);
            }
        }
        std::sort(list.begin(), list.end(), std::greater<>{});
        std::string out;
        for (size_t i = 0; i < std::min(top, list.size()); ++i) {
            out += fmt::format("{}{:#04x}={:.0f}", out.empty() ? "" : " ", list[i].second,
                               double(list[i].first) / std::max(frames, 1u));
        }
        return out.empty() ? "none" : out;
    }

    /// Assembly-line planning statistics, as counts per frame.
    static void DumpPipe(u32 frames) {
        auto& p = VideoCore::ReadbackTrace::Pipe();
        const double f = std::max(frames, 1u);
        const auto per = [f](std::atomic<u64>& c) { return double(c.exchange(0)) / f; };
        using VideoCore::ReadbackTrace::EpochSource;
        const auto src = [&](EpochSource e) { return per(p.epoch_by_source[size_t(e)]); };
        LOG_INFO(Render,
                 "[pipe-trace] per_frame: frames={} gfx_submits={:.1f} asc_submits={:.1f} "
                 "commands={:.1f} blocking_commands={:.1f} flushes={:.1f} finishes={:.1f} "
                 "waits={:.1f} render_begins={:.0f} render_ends={:.0f} stream_epoch_misses={:.0f} "
                 "stream_epoch_miss_kb={:.0f} epoch_src pkt={:.0f} newbuf={:.0f} cmd={:.1f} "
                 "writeback={:.1f} map={:.1f} unmark={:.1f} constram={:.0f}",
                 frames, per(p.gfx_submits), per(p.asc_submits), per(p.commands),
                 per(p.blocking_commands), per(p.flushes), per(p.finishes), per(p.waits),
                 per(p.render_begins), per(p.render_ends), per(p.stream_epoch_misses),
                 per(p.stream_epoch_miss_bytes) / 1024.0, src(EpochSource::Packet),
                 src(EpochSource::NewBuffer), src(EpochSource::Command),
                 src(EpochSource::WriteBack), src(EpochSource::MapChange),
                 src(EpochSource::Unmark), src(EpochSource::ConstRam));
        LOG_INFO(Render,
                 "[pipe-trace] copy_worker per_frame: copies={:.0f} copy_kb={:.0f} "
                 "full_drains={:.0f} overlap_drains={:.0f} wait_us={:.0f}",
                 per(p.worker_copies), per(p.worker_copy_bytes) / 1024.0,
                 per(p.worker_full_drains), per(p.worker_overlap_drains),
                 per(p.worker_wait_ns) / 1000.0);
        LOG_INFO(Render, "[pipe-trace] gfx_ops_per_frame: {}", TopOps(p.gfx_ops, frames, 24));
        LOG_INFO(Render, "[pipe-trace] asc_ops_per_frame: {}", TopOps(p.asc_ops, frames, 12));
        LOG_INFO(Render, "[pipe-trace] ce_ops_per_frame: {}", TopOps(p.ce_ops, frames, 8));
    }

    void Dump() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_dump < std::chrono::seconds(2)) {
            return;
        }
        const double secs = std::chrono::duration<double>(now - last_dump).count();
        const u32 flips = DebugState.GetFrameNum();
        const std::string opts_now = ActiveOpts();
        last_dump = now;
        auto& c = VideoCore::ReadbackTrace::Counters();
        LOG_INFO(Render,
                 "[barrier-trace] fps={:.1f} draws_per_frame={:.0f} dispatches_per_frame={:.0f} "
                 "opts={} full_calls={} partial_calls={} memo_hits={} "
                 "subresources_visited={} barriers={} find_hits={} find_misses={} "
                 "registry_changes={} access_adds={} access_new_ranges={} desc_hits={} "
                 "desc_misses={} desc_copy_skips={} stream_copies={} stream_copy_kb={} stream_reuses={} "
                 "stream_reuse_kb={} epoch_bumps={}",
                 (flips - last_flips) / secs, DebugState.GetDrawCallsAvg(),
                 DebugState.GetDispatchesAvg(),
                 opts_at_start == opts_now ? opts_now : std::string{"mixed"},
                 full_calls.exchange(0), partial_calls.exchange(0), memo_hits.exchange(0),
                 visited.exchange(0), barriers.exchange(0), c.find_hits.exchange(0),
                 c.find_misses.exchange(0), c.registry_changes.exchange(0),
                 c.access_adds.exchange(0), c.access_merges.exchange(0), c.desc_hits.exchange(0),
                 c.desc_misses.exchange(0), c.desc_copy_skips.exchange(0),
                 c.stream_copies.exchange(0),
                 c.stream_copy_bytes.exchange(0) / 1024, c.stream_reuses.exchange(0),
                 c.stream_reuse_bytes.exchange(0) / 1024, c.epoch_bumps.exchange(0));
        DumpPipe(flips - last_flips);
        last_flips = flips;
        opts_at_start = opts_now;
        std::vector<std::pair<const void*, PerImage>> top(images.begin(), images.end());
        std::sort(top.begin(), top.end(),
                  [](const auto& a, const auto& b) { return a.second.visited > b.second.visited; });
        for (size_t i = 0; i < std::min<size_t>(top.size(), 6); ++i) {
            const auto& p = top[i].second;
            LOG_INFO(Render,
                     "[barrier-trace]   image {}x{}x{} type={} levels={} layers={} calls={} "
                     "hits={} visited={} last_range=mip {}+{} layer {}+{}",
                     p.width, p.height, p.depth, p.type, p.levels, p.layers, p.calls, p.hits,
                     p.visited, p.range.base.level, p.range.extent.levels, p.range.base.layer,
                     p.range.extent.layers);
        }
        images.clear();
    }
};

BarrierStats& GetBarrierStats() {
    static BarrierStats stats;
    return stats;
}

} // Anonymous namespace

void Image::GetBarriers(Barriers& barriers, BackingImage& image_backing, vk::ImageLayout dst_layout,
                        vk::AccessFlags2 dst_mask, vk::PipelineStageFlags2 dst_stage,
                        std::optional<SubresourceRange> subres_range) {
    auto& last_state = image_backing.state;
    auto& subresource_states = image_backing.subresource_states;

    const bool needs_partial_transition =
        subres_range &&
        (subres_range->base != SubresourceBase{} || subres_range->extent != info.resources);
    const bool partially_transited = !subresource_states.empty();

    constexpr auto memo_write_flags = vk::AccessFlagBits2::eTransferWrite |
                                      vk::AccessFlagBits2::eShaderWrite |
                                      vk::AccessFlagBits2::eMemoryWrite;
    const bool tracing = VideoCore::ReadbackTrace::TraceBarriers();
    const size_t barriers_before = barriers.size();

    // Local optimization: a read-only partial transition that already ran with no state change
    // since then would visit every subresource in the range and find nothing to do. Skip the walk
    // (last_state is still updated below, exactly as the walk would leave it).
    const bool use_memo = VideoCore::ReadbackTrace::BarrierMemo() && needs_partial_transition &&
                          !(dst_mask & memo_write_flags);
    if (use_memo && partially_transited) {
        for (const auto& memo : image_backing.noop_memo) {
            if (memo.version == image_backing.state_version && memo.range == *subres_range &&
                memo.layout == dst_layout && memo.access_mask == dst_mask) {
                if (tracing) {
                    auto& s = GetBarrierStats();
                    VideoCore::ReadbackTrace::Add(s.partial_calls, 1);
                    VideoCore::ReadbackTrace::Add(s.memo_hits, 1);
                    s.MaybeDump();
                }
                last_state.layout = dst_layout;
                last_state.access_mask = dst_mask;
                last_state.pl_stage = dst_stage;
                return;
            }
        }
    }

    if (needs_partial_transition || partially_transited) {
        if (!partially_transited) {
            subresource_states.resize(info.resources.levels * info.resources.layers);
            std::fill(subresource_states.begin(), subresource_states.end(), last_state);
            ++image_backing.state_version;
        }

        // In case of partial transition, we need to change the specified subresources only.
        // Otherwise all subresources need to be set to the same state so we can use a full
        // resource transition for the next time.
        const u32 start_mip = needs_partial_transition ? subres_range->base.level : u16{0};
        const u32 end_mip = needs_partial_transition
                                ? subres_range->base.level + subres_range->extent.levels
                                : info.resources.levels;
        const u32 start_layer = needs_partial_transition ? subres_range->base.layer : u16{0};
        const u32 end_layer = needs_partial_transition
                                  ? subres_range->base.layer + subres_range->extent.layers
                                  : info.resources.layers;
        for (u32 mip = start_mip; mip < end_mip; ++mip) {
            for (u32 layer = start_layer; layer < end_layer; ++layer) {
                // NOTE: these loops may produce a lot of small barriers.
                // If this becomes a problem, we can optimize it by merging adjacent barriers.
                const auto subres_idx = mip * info.resources.layers + layer;
                ASSERT(subres_idx < subresource_states.size());
                auto& state = subresource_states[subres_idx];

                constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                             vk::AccessFlagBits2::eShaderWrite |
                                             vk::AccessFlagBits2::eMemoryWrite;
                const bool is_write = static_cast<bool>(state.access_mask & write_flags);
                if (state.layout != dst_layout || state.access_mask != dst_mask || is_write) {
                    barriers.emplace_back(vk::ImageMemoryBarrier2{
                        .srcStageMask = state.pl_stage,
                        .srcAccessMask = state.access_mask,
                        .dstStageMask = dst_stage,
                        .dstAccessMask = dst_mask,
                        .oldLayout = state.layout,
                        .newLayout = dst_layout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = image_backing.image.image,
                        .subresourceRange{
                            .aspectMask = aspect_mask,
                            .baseMipLevel = mip,
                            .levelCount = 1,
                            .baseArrayLayer = layer,
                            .layerCount = 1,
                        },
                    });
                    state.layout = dst_layout;
                    state.access_mask = dst_mask;
                    state.pl_stage = dst_stage;
                }
            }
        }

        if (barriers.size() != barriers_before) {
            ++image_backing.state_version;
        }
        if (tracing) {
            auto& s = GetBarrierStats();
            const u64 visited = u64(end_mip - start_mip) * (end_layer - start_layer);
            VideoCore::ReadbackTrace::Add(s.partial_calls, 1);
            VideoCore::ReadbackTrace::Add(s.visited, visited);
            VideoCore::ReadbackTrace::Add(s.barriers, barriers.size() - barriers_before);
            std::scoped_lock lk{s.mutex};
            auto& p = s.images[this];
            ++p.calls;
            p.visited += visited;
            p.levels = info.resources.levels;
            p.layers = info.resources.layers;
            p.range = subres_range ? *subres_range : SubresourceRange{};
            p.width = info.size.width;
            p.height = info.size.height;
            p.depth = info.size.depth;
            p.type = static_cast<u32>(info.type);
            s.Dump();
        }

        if (!needs_partial_transition) {
            subresource_states.clear();
            ++image_backing.state_version;
        } else if (use_memo) {
            // Every subresource in the range is now in (dst_layout, dst_mask) without write
            // access, so repeating this exact transition is a no-op until the state changes.
            auto& memo = image_backing.noop_memo[image_backing.noop_memo_next++ %
                                                 image_backing.noop_memo.size()];
            memo = NoopTransition{
                .range = *subres_range,
                .layout = dst_layout,
                .access_mask = dst_mask,
                .version = image_backing.state_version,
            };
        }
    } else { // Full resource transition
        if (tracing) {
            auto& s = GetBarrierStats();
            VideoCore::ReadbackTrace::Add(s.full_calls, 1);
            s.MaybeDump();
        }
        constexpr auto write_flags = vk::AccessFlagBits2::eTransferWrite |
                                     vk::AccessFlagBits2::eShaderWrite |
                                     vk::AccessFlagBits2::eMemoryWrite;
        const bool is_write = static_cast<bool>(last_state.access_mask & write_flags);
        if (last_state.layout == dst_layout && last_state.access_mask == dst_mask && !is_write) {
            return;
        }
        barriers.emplace_back(vk::ImageMemoryBarrier2{
            .srcStageMask = last_state.pl_stage,
            .srcAccessMask = last_state.access_mask,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_mask,
            .oldLayout = last_state.layout,
            .newLayout = dst_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image_backing.image.image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        });
        ++image_backing.state_version;
    }

    last_state.layout = dst_layout;
    last_state.access_mask = dst_mask;
    last_state.pl_stage = dst_stage;
}

} // namespace VideoCore
