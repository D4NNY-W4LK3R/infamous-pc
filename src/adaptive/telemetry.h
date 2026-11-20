// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Telemetry service (Architecture v1.0 shared service).
//
// The flip thread publishes one FrameSample per presented frame into a fixed single-producer /
// single-consumer ring: two relaxed loads, one copy and one release store, no lock, no
// allocation. The brain thread drains it about once a second. If the brain falls behind, the
// oldest unread samples are overwritten and counted as dropped - the game never waits.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "adaptive/contracts.h"

namespace Adaptive {

class FrameRing {
public:
    static constexpr std::size_t Capacity = 4096; // ~68 s at 60 fps, ~136 s at 30 fps

    /// Producer side (flip thread only).
    void Push(const FrameSample& sample) noexcept;
    /// Consumer side (brain thread only). Appends everything published since the last call.
    /// Returns the number of samples that were overwritten before they could be read.
    std::uint64_t Drain(std::vector<FrameSample>& out);

    std::uint64_t Published() const noexcept {
        return head.load(std::memory_order_acquire);
    }

private:
    std::array<FrameSample, Capacity> slots{};
    alignas(64) std::atomic<std::uint64_t> head{0}; ///< written by the producer
    alignas(64) std::uint64_t tail = 0;             ///< consumer only
};

/// One second of frames condensed; the unit of every before/after comparison.
struct SecondSummary {
    std::uint64_t start_ns = 0;
    std::uint32_t frames = 0;
    double median_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
    std::uint32_t hitches = 0;
    std::array<double, NumStallKinds> stall_ms{}; ///< summed over the second
    std::uint32_t draws_median = 0;
    std::uint32_t dispatches_median = 0;
    /// Every frame of the second drew exactly the same (a still picture).
    bool same_every_frame = false;
    /// Identifies a still picture across seconds and launches (0 = not a still picture).
    std::uint64_t signature = 0;
    double gpu_busy_median_ms = 0.0;
    /// Frames of 100 ms or more, and the time they took beyond the second's median frame.
    std::uint32_t freezes = 0;
    double frozen_ms = 0.0;
};

/// Keeps a rolling window of frames and per-second summaries and builds Snapshots from them.
class TelemetryWindow {
public:
    explicit TelemetryWindow(double window_seconds = 10.0) : window_seconds{window_seconds} {}

    void Add(const std::vector<FrameSample>& samples);
    void SetResources(const ResourceSample& r) {
        resources = r;
    }

    /// Statistics over the frames of the last window_seconds.
    Snapshot Build(std::uint64_t now_ns) const;

    /// Seconds completed since (and including) the one starting at or after since_ns.
    std::vector<SecondSummary> SecondsSince(std::uint64_t since_ns) const;
    const std::deque<SecondSummary>& Seconds() const {
        return seconds;
    }
    std::uint64_t TotalFrames() const {
        return total_frames;
    }

private:
    void CloseSecond();

    double window_seconds;
    std::deque<FrameSample> frames;
    std::deque<SecondSummary> seconds; ///< at most MaxSeconds
    std::vector<FrameSample> current_second;
    std::uint64_t current_second_start = 0;
    ResourceSample resources{};
    std::uint64_t total_frames = 0;
    static constexpr std::size_t MaxSeconds = 1800; // 30 minutes of history
};

/// A frame is a hitch when it is at least this long and at least twice the median.
inline constexpr double HitchFloorMs = 25.0;

} // namespace Adaptive
