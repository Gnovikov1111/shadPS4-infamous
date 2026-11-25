// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <iterator>
#include <vector>
#include <absl/container/btree_map.h>
#include "common/interval_set.h"
#include "common/types.h"
#include "video_core/readback_trace.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/types.h"

namespace VideoCore {
class BlitHelper;
} // namespace VideoCore

namespace Vulkan {

class Instance;
class Scheduler;

class Runtime {
public:
    explicit Runtime(const Instance& instance, Scheduler& scheduler);
    ~Runtime() = default;

    const Instance& GetInstance() const {
        return instance;
    }

    StagingBufferPool& GetStagingPool() {
        return staging_pool;
    }

    void TickFrame();

    void CopyBuffer(const VideoCore::Buffer* src, const VideoCore::Buffer* dst,
                    std::span<const vk::BufferCopy> copies);

    void FillBuffer(const VideoCore::Buffer* dst, u64 offset, u64 size, u32 value);

    void InlineData(VideoCore::Buffer* dst, u64 offset, u32 value);

    bool Transit(VideoCore::Image* image, vk::ImageLayout dst_layout,
                 vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                 std::optional<VideoCore::SubresourceRange> subres_range = {});

    void UploadImage(VideoCore::Image* dst, const VideoCore::Buffer* src,
                     std::span<const vk::BufferImageCopy> upload_copies);
    void DownloadImage(VideoCore::Image* src, const VideoCore::Buffer* dst,
                       std::span<const vk::BufferImageCopy> download_copies);

    void CopyImage(VideoCore::Image* src, VideoCore::Image* dst);
    VideoCore::ImageView& FindSubrectView(VideoCore::Image& image, const VideoCore::ImageInfo& info,
                                          const VideoCore::ImageViewInfo& view_info);
    void CopyImageWithBuffer(VideoCore::Image* src, VideoCore::Image* dst,
                             const VideoCore::Buffer* buffer, u64 offset);
    void CopyMip(VideoCore::Image* src, VideoCore::Image* dst, u32 mip, u32 slice);

    void CopyColorAndDepth(VideoCore::Image* src, VideoCore::Image* dst);

    void CopyDepthStencil(VideoCore::Image* src, VideoCore::Image* dst,
                          const VideoCore::SubresourceRange& sub_range);

    void ResolveImage(VideoCore::Image* src, VideoCore::Image* dst,
                      const VideoCore::SubresourceRange& src_range,
                      const VideoCore::SubresourceRange& dst_range);
    void ClearImage(VideoCore::Image* dst, const VideoCore::SubresourceRange& range,
                    const vk::ClearValue& clear_value);

    void SetBackingSamples(VideoCore::Image* image, u32 num_samples, bool copy_backing = true);

    void AccessBuffer(const VideoCore::Buffer* handle, u64 offset, u64 size,
                      vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access);

    bool IsBufferAccessed(const VideoCore::Buffer* handle, u64 offset, u64 size,
                          bool check_read_access = false);

    void FlushBarriers();

private:
    void MakeCurrent(const VideoCore::Buffer* handle);

private:
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VideoCore::BlitHelper> blit_helper;
    StagingBufferPool staging_pool;
    /// Local optimization (SHADPS4_FAST_ACCESS_LISTS): answers Overlaps exactly like
    /// IntervalList, but keeps the disjoint ranges in a B-tree (start -> end), so adding a range
    /// costs O(log n) instead of shifting a sorted array that can hold 100k+ ranges per frame.
    /// With SetLazy (SHADPS4_LAZY_READ_RANGES), new ranges are only appended to a plain list and
    /// folded into the tree when Overlaps needs them, so lists that are never queried before the
    /// next barrier flush (read ranges of buffers the GPU never writes) cost O(1) per add.
    class BatchedIntervals {
    public:
        void SetLazy(bool value) {
            lazy = value;
        }
        void Add(u64 start, u64 end) {
            if (start >= end) [[unlikely]] {
                return;
            }
            if (lazy) {
                pending.push_back(Interval{start, end});
                if (pending.size() >= MaxPending) {
                    Fold();
                }
                return;
            }
            Insert(start, end);
        }
        bool Overlaps(u64 start, u64 end) {
            if (start >= end) [[unlikely]] {
                return false;
            }
            if (!pending.empty()) {
                Fold();
            }
            const auto it = ranges.upper_bound(start);
            if (it != ranges.begin() && std::prev(it)->second > start) {
                return true;
            }
            return it != ranges.end() && it->first < end;
        }
        void Clear() {
            ranges.clear();
            pending.clear();
        }

    private:
        // Upper bound on unsorted ranges kept between barrier flushes (16 MB). It must be large:
        // the per-draw stream buffer alone records ~70k ranges per frame that are never queried.
        static constexpr size_t MaxPending = size_t{1} << 20;

        void Fold() {
            // Sort and join the pending ranges first: most are repeats or neighbours, so far
            // fewer tree inserts remain. The union is unchanged.
            std::ranges::sort(pending, {}, &Interval::start);
            u64 cur_start = pending.front().start;
            u64 cur_end = pending.front().end;
            for (size_t i = 1; i < pending.size(); ++i) {
                if (pending[i].start <= cur_end) {
                    cur_end = std::max(cur_end, pending[i].end);
                } else {
                    Insert(cur_start, cur_end);
                    cur_start = pending[i].start;
                    cur_end = pending[i].end;
                }
            }
            Insert(cur_start, cur_end);
            pending.clear();
        }

        void Insert(u64 start, u64 end) {
            auto it = ranges.upper_bound(start); // first range starting after `start`
            if (it != ranges.begin()) {
                const auto prev = std::prev(it);
                if (prev->second >= end) {
                    return; // already covered (the common case: same buffers every draw)
                }
                if (prev->second >= start) {
                    // Overlaps or touches the previous range: absorb it.
                    start = prev->first;
                    it = ranges.erase(prev);
                }
            }
            while (it != ranges.end() && it->first <= end) {
                end = std::max(end, it->second);
                it = ranges.erase(it);
            }
            ranges.emplace_hint(it, start, end);
            VideoCore::ReadbackTrace::Count(VideoCore::ReadbackTrace::Counters().access_merges);
        }

        absl::btree_map<u64, u64> ranges; // disjoint, non-touching [start, end)
        std::vector<Interval> pending;    // unsorted, not yet in `ranges` (lazy mode only)
        bool lazy{};
    };
    struct BufferBarriers {
        const VideoCore::Buffer* handle;
        using AccessList = IntervalList<Interval>;
        AccessList read_ranges;
        AccessList write_ranges;
        bool batched{};
        BatchedIntervals batched_read;
        BatchedIntervals batched_write;
    };
    BufferBarriers* resource{};
    std::vector<BufferBarriers> resources;
    VideoCore::Image::Barriers image_barriers;
    vk::MemoryBarrier2 memory_barrier{};
};

} // namespace Vulkan
