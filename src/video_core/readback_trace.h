// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Local investigation helpers for GPU -> CPU readback behaviour (inFAMOUS Second Son smoke).
// Everything here is off unless the matching environment variable is set, so a build with this
// file behaves exactly like upstream by default.
//
//   SHADPS4_TRACE_READBACKS=1   log guest page-fault and download statistics every ~2 seconds
//   SHADPS4_EOP_FINISH=1        wait for the host GPU before signalling EOP/EOS/ReleaseMem fences
//   SHADPS4_COPY_RESERVE_MB=N   reserve N MB per queue for "Copy GPU Command Buffers" (default 2)
//   SHADPS4_BARRIER_MEMO=1      skip repeated no-op partial image transitions (perf)
//   SHADPS4_FIND_IMAGE_CACHE=1  cache exact-match texture lookups (perf)
//   SHADPS4_FAST_ACCESS_LISTS=1 B-tree buffer access range tracking (perf)
//   SHADPS4_LAZY_READ_RANGES=1  defer sorting access ranges until a hazard check needs them (perf)
//   SHADPS4_IMAGE_DESC_CACHE=1  reuse texture descriptions per 32-byte T# (perf)
//   SHADPS4_STREAM_DEDUPE=1     reuse a small read-only buffer's stream copy while guest memory
//                               cannot have changed (perf)
//   SHADPS4_DESC_SLOT_REUSE=1   skip re-copying a cached texture description into its slot (perf)
//   SHADPS4_COPY_WORKER=1       assembly line stage 1: guest-memory copies into stream / upload
//                               buffers run on a worker thread (perf, see copy_worker.h)
//                               (perf switches accept 2 = A/B: on 10 s, off 10 s)
//   SHADPS4_TRACE_BARRIERS=1    log image barrier tracking statistics every ~2 seconds

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "common/types.h"

namespace VideoCore::ReadbackTrace {

inline bool EnvFlag(const char* name) {
    const char* value = std::getenv(name);
    return value && *value && *value != '0';
}

inline u64 EnvNumber(const char* name, u64 fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) {
        return fallback;
    }
    const u64 parsed = std::strtoull(value, nullptr, 10);
    return parsed ? parsed : fallback;
}

inline bool Enabled() {
    static const bool enabled = EnvFlag("SHADPS4_TRACE_READBACKS");
    return enabled;
}

inline bool EopFinish() {
    static const bool enabled = EnvFlag("SHADPS4_EOP_FINISH");
    return enabled;
}

/// SHADPS4_FAULT_WIDEN_PAGES=N: on a CPU write fault, also release the next N-1 pages that the GPU
/// has not modified (0 or 1 = upstream behaviour).
inline u64 FaultWidenPages() {
    static const u64 pages = EnvNumber("SHADPS4_FAULT_WIDEN_PAGES", 0);
    return pages;
}

/// Mode value of a perf switch: 0 = off, 1 = on, 2 = A/B (on for 10 s, off for 10 s, ...).
inline bool PerfSwitchActive(u64 mode) {
    if (mode == 2) {
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
        return (secs / 10) % 2 == 0;
    }
    return mode != 0;
}

/// SHADPS4_BARRIER_MEMO=1: skip re-walking every subresource for repeated partial image
/// transitions that are known to be no-ops (see Image::GetBarriers). 2 = A/B mode.
inline bool BarrierMemo() {
    static const u64 mode = EnvNumber("SHADPS4_BARRIER_MEMO", 0);
    return PerfSwitchActive(mode);
}

/// SHADPS4_FIND_IMAGE_CACHE=1: remember exact-match TextureCache::FindImage results until an
/// image is registered or unregistered. 2 = A/B mode.
inline bool FindImageCache() {
    static const u64 mode = EnvNumber("SHADPS4_FIND_IMAGE_CACHE", 0);
    return PerfSwitchActive(mode);
}

/// SHADPS4_FAST_ACCESS_LISTS=1: record buffer access ranges in batches instead of inserting
/// each one into a sorted array (Runtime::AccessBuffer). 2 = A/B mode.
inline bool FastAccessLists() {
    static const u64 mode = EnvNumber("SHADPS4_FAST_ACCESS_LISTS", 0);
    return PerfSwitchActive(mode);
}

/// SHADPS4_LAZY_READ_RANGES=1: with SHADPS4_FAST_ACCESS_LISTS, append access ranges to a plain
/// list and sort them into the tree only when a hazard check needs them. 2 = A/B mode.
inline bool LazyReadRanges() {
    static const u64 mode = EnvNumber("SHADPS4_LAZY_READ_RANGES", 0);
    return PerfSwitchActive(mode);
}

/// SHADPS4_IMAGE_DESC_CACHE=1: reuse the texture description built from a T# (sizes, mip layout,
/// view) when the same 32-byte descriptor is bound again. 2 = A/B mode.
inline bool ImageDescCache() {
    static const u64 mode = EnvNumber("SHADPS4_IMAGE_DESC_CACHE", 0);
    return PerfSwitchActive(mode);
}

/// SHADPS4_DESC_SLOT_REUSE=1: with SHADPS4_IMAGE_DESC_CACHE, skip copying a cached texture
/// description into a binding slot that already holds an unmodified copy of it. 2 = A/B mode.
inline bool DescSlotReuse() {
    static const u64 mode = EnvNumber("SHADPS4_DESC_SLOT_REUSE", 0);
    return PerfSwitchActive(mode);
}

/// SHADPS4_STREAM_DEDUPE=1: when the same small read-only buffer (same address and size) is bound
/// again and guest memory cannot have changed since its last copy into the stream buffer (no
/// GuestMemoryEpoch bump, no stream buffer wrap), bind that copy again instead of copying it once
/// more. 2 = A/B mode.
inline bool StreamDedupe() {
    static const u64 mode = EnvNumber("SHADPS4_STREAM_DEDUPE", 0);
    return PerfSwitchActive(mode);
}

/// SHADPS4_TRACE_BARRIERS=1: log image barrier tracking statistics every ~2 seconds.
inline bool TraceBarriers() {
    static const bool enabled = EnvFlag("SHADPS4_TRACE_BARRIERS");
    return enabled;
}

/// Counters for the perf switches above (only updated while SHADPS4_TRACE_BARRIERS is set).
struct PerfCounters {
    std::atomic<u64> find_hits{};
    std::atomic<u64> find_misses{};
    std::atomic<u64> registry_changes{};
    std::atomic<u64> access_adds{};
    std::atomic<u64> access_merges{};
    std::atomic<u64> desc_hits{};
    std::atomic<u64> desc_misses{};
    std::atomic<u64> desc_copy_skips{};
    std::atomic<u64> stream_copies{};
    std::atomic<u64> stream_copy_bytes{};
    std::atomic<u64> stream_reuses{};
    std::atomic<u64> stream_reuse_bytes{};
    std::atomic<u64> epoch_bumps{};
};

inline PerfCounters& Counters() {
    static PerfCounters counters;
    return counters;
}

/// Statistics increment without a locked instruction (a plain load and store): a count may be
/// lost if two threads race, which is fine for statistics and far cheaper on the GPU thread.
inline void Add(std::atomic<u64>& counter, u64 n = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline void Count(std::atomic<u64>& counter, u64 n = 1) {
    if (TraceBarriers()) {
        Add(counter, n);
    }
}

/// Changes whenever guest memory may hold new data that a later draw must see: a write by the
/// command processor or by a GPU -> CPU readback, a wait that lets the guest finish writing, a
/// new command buffer, a memory map change, or a queued GPU-thread command. Used by
/// SHADPS4_STREAM_DEDUPE, which only reuses stream copies made in the current epoch.
inline std::atomic<u64>& GuestMemoryEpoch() {
    static std::atomic<u64> epoch{1};
    return epoch;
}

/// What changed guest memory (for SHADPS4_TRACE_BARRIERS statistics).
enum class EpochSource : u32 {
    Packet,    // a PM4 packet that may write guest memory or wait for the guest
    NewBuffer, // start of a command buffer (top level or indirect)
    Command,   // a queued GPU-thread command (Liverpool::ProcessCommands)
    WriteBack, // GPU -> guest memory write (fence labels, readbacks)
    MapChange, // guest memory mapped or unmapped
    Unmark,    // GPU-modified range handed back to the CPU
    ConstRam,  // constant engine DumpConstRam
    Count,
};

/// Assembly-line planning counters (only updated while SHADPS4_TRACE_BARRIERS is set): how
/// often the GPU thread reaches points where a pipelined back-end would have to catch up.
struct PipeCounters {
    std::array<std::atomic<u64>, 256> gfx_ops{};
    std::array<std::atomic<u64>, 256> asc_ops{};
    std::array<std::atomic<u64>, 256> ce_ops{};
    std::array<std::atomic<u64>, static_cast<size_t>(EpochSource::Count)> epoch_by_source{};
    std::atomic<u64> commands{};          // ProcessCommands callbacks run
    std::atomic<u64> blocking_commands{}; // SendCommand<true>: the calling thread waits
    std::atomic<u64> gfx_submits{};
    std::atomic<u64> asc_submits{};
    std::atomic<u64> flushes{};       // Scheduler::SubmitExecution
    std::atomic<u64> finishes{};      // Scheduler::Finish (GPU drain)
    std::atomic<u64> waits{};         // Scheduler::Wait on a tick the GPU had not reached yet
    std::atomic<u64> render_begins{}; // vkCmdBeginRendering
    std::atomic<u64> render_ends{};   // vkCmdEndRendering
    std::atomic<u64> stream_epoch_misses{}; // stream copy not reused only because the epoch moved
    std::atomic<u64> stream_epoch_miss_bytes{};
    std::atomic<u64> worker_copies{};         // SHADPS4_COPY_WORKER: copies handed to the worker
    std::atomic<u64> worker_copy_bytes{};
    std::atomic<u64> worker_full_drains{};    // waits for all queued copies
    std::atomic<u64> worker_overlap_drains{}; // waits because a queued copy reads written memory
    std::atomic<u64> worker_wait_ns{};        // time the waiting thread spent in those waits
};

inline PipeCounters& Pipe() {
    static PipeCounters counters;
    return counters;
}

inline void BumpGuestMemoryEpoch(EpochSource source) {
    GuestMemoryEpoch().fetch_add(1, std::memory_order_acq_rel);
    if (TraceBarriers()) {
        Add(Counters().epoch_bumps);
        Add(Pipe().epoch_by_source[static_cast<size_t>(source)]);
    }
}

/// GPU-thread watchdog (always on, cheap): what the GPU command thread is waiting on, and a
/// counter of PM4 packets it processed. Liverpool's watchdog thread logs both if the counter
/// stops moving while the thread is not idle.
enum class GpuWait : u32 {
    Running,
    Idle,         // nothing submitted
    CopyDrain,    // waiting for SHADPS4_COPY_WORKER copies
    GuestWait,    // WaitRegMem polling
    VoLabel,      // sleeping on a video-out label
    GpuSemaphore, // waiting for the host GPU (Scheduler::Wait / Finish)
    FrameWait,    // presenter waiting for a free frame
    Command,      // running a queued command (readback etc.)
};

inline const char* GpuWaitName(u32 wait) {
    constexpr const char* names[] = {"Running",   "Idle",         "CopyDrain", "GuestWait",
                                     "VoLabel",   "GpuSemaphore", "FrameWait", "Command"};
    return wait < std::size(names) ? names[wait] : "?";
}

inline std::atomic<u32>& GpuWaitState() {
    static std::atomic<u32> state{0};
    return state;
}

inline std::atomic<u64>& GpuWaitAddr() {
    static std::atomic<u64> addr{0};
    return addr;
}

inline std::atomic<u64>& GpuProgress() {
    static std::atomic<u64> progress{0};
    return progress;
}

inline std::atomic<std::thread::id>& GpuThreadId() {
    static std::atomic<std::thread::id> id{};
    return id;
}

/// Marks a wait of the GPU command thread (no-op on other threads); back to Running on exit.
class GpuWaitScope {
public:
    explicit GpuWaitScope(GpuWait wait, u64 addr = 0)
        : active{std::this_thread::get_id() == GpuThreadId().load(std::memory_order_relaxed)} {
        if (active) {
            GpuWaitAddr().store(addr, std::memory_order_relaxed);
            GpuWaitState().store(static_cast<u32>(wait), std::memory_order_relaxed);
        }
    }
    ~GpuWaitScope() {
        if (active) {
            GpuWaitState().store(static_cast<u32>(GpuWait::Running), std::memory_order_relaxed);
        }
    }
    GpuWaitScope(const GpuWaitScope&) = delete;
    GpuWaitScope& operator=(const GpuWaitScope&) = delete;

private:
    bool active;
};

inline u64 CopyReserveBytes() {
    static const u64 bytes = EnvNumber("SHADPS4_COPY_RESERVE_MB", 2) * 1024ULL * 1024ULL;
    return bytes;
}

struct PageStats {
    u32 reads{};
    u32 writes{};
    u32 gpu_thread{};
};

struct State {
    std::mutex mutex;
    std::unordered_map<u64, PageStats> pages;
    u64 read_faults{};
    u64 write_faults{};
    u64 read_fault_ns{};
    u64 write_fault_ns{};
    u64 compiles{};
    u64 compile_ns{};
    u64 worst_compile_ns{};
    u64 finishes{};
    u64 finish_ns{};
    u64 downloads{};
    u64 download_bytes{};
    u64 fences{};
    u64 submits{};
    std::chrono::steady_clock::time_point last_dump = std::chrono::steady_clock::now();
};

inline State& Get() {
    static State state;
    return state;
}

inline void RecordFault(u64 addr, bool is_write, bool is_gpu_thread, u64 handler_ns = 0) {
    if (!Enabled()) {
        return;
    }
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    auto& page = s.pages[addr >> 12];
    if (is_write) {
        ++page.writes;
        ++s.write_faults;
        s.write_fault_ns += handler_ns;
    } else {
        ++page.reads;
        ++s.read_faults;
        s.read_fault_ns += handler_ns;
    }
    if (is_gpu_thread) {
        ++page.gpu_thread;
    }
}

inline void RecordDownload(u64 bytes) {
    if (!Enabled()) {
        return;
    }
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    ++s.downloads;
    s.download_bytes += bytes;
}

inline void RecordCompile(u64 ns) {
    if (!Enabled()) {
        return;
    }
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    ++s.compiles;
    s.compile_ns += ns;
    s.worst_compile_ns = std::max(s.worst_compile_ns, ns);
}

/// Host GPU drains (Scheduler::Finish): CPU and GPU both stall until queued work completes.
inline void RecordFinish(u64 ns) {
    if (!Enabled()) {
        return;
    }
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    ++s.finishes;
    s.finish_ns += ns;
}

inline void RecordFence() {
    if (!Enabled()) {
        return;
    }
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    ++s.fences;
}

/// Called once per GPU submit; prints a summary roughly every two seconds.
inline void Tick() {
    if (!Enabled()) {
        return;
    }
    auto& s = Get();
    std::scoped_lock lk{s.mutex};
    ++s.submits;
    const auto now = std::chrono::steady_clock::now();
    if (now - s.last_dump < std::chrono::seconds(2)) {
        return;
    }
    s.last_dump = now;
    std::vector<std::pair<u64, PageStats>> top(s.pages.begin(), s.pages.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
        return (a.second.reads + a.second.writes) > (b.second.reads + b.second.writes);
    });
    const auto wall = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    LOG_INFO(Render,
             "[readback-trace] t={} submits={} fences={} read_faults={} write_faults={} pages={} "
             "downloads={} download_kb={} read_fault_ms={} write_fault_ms={} compiles={} "
             "compile_ms={} worst_compile_ms={} gpu_drains={} gpu_drain_ms={}",
             wall, s.submits, s.fences, s.read_faults, s.write_faults, s.pages.size(), s.downloads,
             s.download_bytes / 1024, s.read_fault_ns / 1000000, s.write_fault_ns / 1000000,
             s.compiles, s.compile_ns / 1000000, s.worst_compile_ns / 1000000, s.finishes,
             s.finish_ns / 1000000);
    const size_t shown = std::min<size_t>(top.size(), 16);
    for (size_t i = 0; i < shown; ++i) {
        LOG_INFO(Render, "[readback-trace]   page {:#x} reads={} writes={} on_gpu_thread={}",
                 top[i].first << 12, top[i].second.reads, top[i].second.writes,
                 top[i].second.gpu_thread);
    }
    s.pages.clear();
    s.read_faults = s.write_faults = s.downloads = s.download_bytes = s.fences = s.submits = 0;
    s.read_fault_ns = s.write_fault_ns = 0;
    s.compiles = s.compile_ns = s.worst_compile_ns = 0;
    s.finishes = s.finish_ns = 0;
}

} // namespace VideoCore::ReadbackTrace
