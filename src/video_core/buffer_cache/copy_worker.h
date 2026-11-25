// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Assembly line, stage 1 (local investigation, SHADPS4_COPY_WORKER):
//
// The GPU command thread copies guest memory into host-visible GPU buffers for almost every draw
// (small vertex/index/constant buffers into the stream buffer, CPU-modified pages into upload
// staging). Those copies are pure: the GPU command thread still decides every copy and reserves
// its destination in order, exactly as before; with the switch on it only hands the memcpy to a
// worker thread instead of doing it inline.
//
// A queued copy must read the same guest bytes it would have read inline and must land before
// the GPU can read its destination. So the GPU command thread waits for queued copies:
//   - before every draw_scheduler submit (Rasterizer submit callback),
//   - before it writes guest memory itself (TryWriteBacking, DMA fill/copy fast paths, constant
//     engine dumps, occlusion results: DrainOverlapping waits only if a queued copy reads there),
//   - before packets that write guest memory or signal / wait for the guest (labels, interrupts,
//     WriteData, ReleaseMem, WaitRegMem, flips...), and before queued GPU-thread commands
//     (readbacks), with a full Drain,
//   - and guest threads wait before guest memory is unmapped (Rasterizer::UnmapMemory).
// Guest threads writing memory that queued draws still read is a guest race, as it is today.

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <immintrin.h>

#include "common/thread.h"
#include "common/types.h"
#include "core/memory.h"
#include "video_core/readback_trace.h"

namespace VideoCore {

class CopyWorker {
public:
    static CopyWorker& Instance() {
        // Never destroyed: the worker thread may still be parked at process exit.
        static CopyWorker* const worker = new CopyWorker();
        return *worker;
    }

    /// Whether new copies should be queued (SHADPS4_COPY_WORKER=1, or 2 = A/B: on 10 s, off 10 s).
    bool Active() const {
        return started && ReadbackTrace::PerfSwitchActive(mode);
    }

    /// Queues CopySparseMemory(src, dst, size). Producer: the GPU command thread only; calls from
    /// any other thread copy inline.
    void Push(VAddr src, u8* dst, u64 size) {
        if (std::this_thread::get_id() != producer) {
            if (producer == std::thread::id{}) {
                producer = std::this_thread::get_id();
            } else {
                Core::Memory::Instance()->CopySparseMemory(src, dst, size);
                return;
            }
        }
        const u64 h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) >= Capacity) [[unlikely]] {
            WaitUntilDone(h - Capacity + 1);
        }
        jobs[h & Mask] = Job{src, dst, size};
        head.store(h + 1, std::memory_order_seq_cst);
        // Only the worker clears `sleeping`. (Clearing it here lost wake-ups: if this thread was
        // delayed between the load and the clear, the worker could finish this job, park again,
        // and then have its flag cleared while parked, so later pushes never notified it.)
        if (sleeping.load(std::memory_order_seq_cst)) {
            head.notify_one();
        }
        ReadbackTrace::Count(ReadbackTrace::Pipe().worker_copies);
        ReadbackTrace::Count(ReadbackTrace::Pipe().worker_copy_bytes, size);
    }

    /// Waits until every copy queued before this call has finished. Any thread.
    void Drain() {
        const u64 target = head.load(std::memory_order_acquire);
        if (tail.load(std::memory_order_acquire) >= target) {
            return;
        }
        ReadbackTrace::Count(ReadbackTrace::Pipe().worker_full_drains);
        WaitUntilDone(target);
    }

    /// Waits until every queued copy that reads guest memory in [addr, addr + size) has
    /// finished (and all copies queued before it). Any thread.
    void DrainOverlapping(VAddr addr, u64 size) {
        const u64 h = head.load(std::memory_order_acquire);
        const u64 t = tail.load(std::memory_order_acquire);
        if (t >= h) {
            return;
        }
        if (h - t > MaxScan) {
            ReadbackTrace::Count(ReadbackTrace::Pipe().worker_full_drains);
            WaitUntilDone(h);
            return;
        }
        const VAddr end = addr + size;
        for (u64 i = h; i-- > t;) {
            const Job& job = jobs[i & Mask];
            if (job.src < end && addr < job.src + job.size) {
                ReadbackTrace::Count(ReadbackTrace::Pipe().worker_overlap_drains);
                WaitUntilDone(i + 1);
                return;
            }
        }
    }

    /// One-line state for hang reports.
    std::string DebugState() const {
        return fmt::format("mode={} head={} tail={} sleeping={} copying_src={:#x}", mode,
                           head.load(), tail.load(), sleeping.load(), copying_src.load());
    }

    /// Wakes the worker (insurance against a missed wake-up).
    void Kick() {
        head.notify_one();
    }

private:
    struct Job {
        VAddr src;
        u8* dst;
        u64 size;
    };
    static constexpr u64 Capacity = u64{1} << 16;
    static constexpr u64 Mask = Capacity - 1;
    static constexpr u64 MaxScan = 4096;

    CopyWorker() : mode{ReadbackTrace::EnvNumber("SHADPS4_COPY_WORKER", 0)} {
        if (mode == 0) {
            return;
        }
        jobs = std::make_unique<Job[]>(Capacity);
        started = true;
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    /// Spins (then yields) until `done` copies have completed.
    void WaitUntilDone(u64 done) {
        const bool tracing = ReadbackTrace::TraceBarriers();
        const auto t0 =
            tracing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        ReadbackTrace::GpuWaitScope wait_scope{ReadbackTrace::GpuWait::CopyDrain, done};
        u32 spins = 0;
        auto slow_since = std::chrono::steady_clock::time_point{};
        bool reported = false;
        const auto wait_start = std::chrono::steady_clock::now();
        while (tail.load(std::memory_order_acquire) < done) {
            if (++spins < 4096) {
                _mm_pause();
                continue;
            }
            std::this_thread::yield();
            if ((spins & 1023) != 0) {
                continue;
            }
            // Watchdog: a drain normally takes microseconds.
            const auto now = std::chrono::steady_clock::now();
            if (slow_since == std::chrono::steady_clock::time_point{}) {
                slow_since = now;
            } else if (now - slow_since > std::chrono::milliseconds(20)) {
                Kick();
                if (!reported) {
                    reported = true;
                    LOG_CRITICAL(Render, "[copy-worker] drain stuck for 20 ms, waiting for {}: {}",
                                 done, DebugState());
                }
            }
        }
        if (reported) {
            LOG_CRITICAL(Render, "[copy-worker] drain finished after {} ms",
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - wait_start)
                             .count());
        }
        if (tracing) {
            ReadbackTrace::Add(ReadbackTrace::Pipe().worker_wait_ns,
                               std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count());
        }
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("shadPS4:CopyWorker");
        // Short bursts the GPU command thread may be waiting on: don't let busy cores (driver
        // shader compiles, game threads) keep it off the CPU.
        Common::SetCurrentThreadPriority(Common::ThreadPriority::VeryHigh);
        auto* memory = Core::Memory::Instance();
        u64 t = tail.load(std::memory_order_relaxed);
        while (!stop.stop_requested()) {
            u64 h = head.load(std::memory_order_acquire);
            if (t == h) {
                // Spin briefly (jobs arrive every microsecond or so in heavy scenes), then park.
                const auto spin_start = std::chrono::steady_clock::now();
                u32 n = 0;
                while ((h = head.load(std::memory_order_acquire)) == t) {
                    _mm_pause();
                    if ((++n & 63) == 0 && std::chrono::steady_clock::now() - spin_start >
                                               std::chrono::microseconds(50)) {
                        // Dekker with Push: either Push sees sleeping=true and notifies, or this
                        // load sees the new head and skips the wait.
                        sleeping.store(true, std::memory_order_seq_cst);
                        h = head.load(std::memory_order_seq_cst);
                        if (h == t) {
                            head.wait(t, std::memory_order_seq_cst);
                        }
                        sleeping.store(false, std::memory_order_seq_cst);
                        break;
                    }
                }
                continue;
            }
            for (; t != h; ++t) {
                const Job& job = jobs[t & Mask];
                copying_src.store(job.src, std::memory_order_relaxed);
                const auto copy_start = std::chrono::steady_clock::now();
                memory->CopySparseMemory(job.src, job.dst, job.size);
                const auto copy_time = std::chrono::steady_clock::now() - copy_start;
                if (copy_time > std::chrono::milliseconds(10)) [[unlikely]] {
                    LOG_WARNING(Render, "[copy-worker] slow copy: src={:#x} size={} took {} ms",
                                job.src, job.size,
                                std::chrono::duration_cast<std::chrono::milliseconds>(copy_time)
                                    .count());
                }
                // The destination is write-combined GPU memory (and memcpy may use streaming
                // stores): those stores are not ordered by a release store, so without a fence
                // the GPU thread could submit while they still sit in this core's buffers.
                _mm_sfence();
                tail.store(t + 1, std::memory_order_release);
            }
        }
    }

    const u64 mode;
    bool started{};
    std::unique_ptr<Job[]> jobs;
    std::thread::id producer{};
    alignas(64) std::atomic<u64> head{0};
    alignas(64) std::atomic<u64> tail{0};
    alignas(64) std::atomic<bool> sleeping{false};
    std::atomic<VAddr> copying_src{0};
    std::jthread thread;
};

} // namespace VideoCore
