// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "adaptive/stats.h"
#include "adaptive/telemetry.h"

namespace Adaptive {

void FrameRing::Push(const FrameSample& sample) noexcept {
    const std::uint64_t h = head.load(std::memory_order_relaxed);
    slots[h % Capacity] = sample;
    head.store(h + 1, std::memory_order_release);
}

std::uint64_t FrameRing::Drain(std::vector<FrameSample>& out) {
    const std::uint64_t h = head.load(std::memory_order_acquire);
    std::uint64_t dropped = 0;
    std::uint64_t start = tail;
    if (h - start > Capacity) {
        dropped += h - Capacity - start;
        start = h - Capacity;
    }
    const std::size_t first_out = out.size();
    for (std::uint64_t i = start; i < h; ++i) {
        out.push_back(slots[i % Capacity]);
    }
    tail = h;
    // While we copied, the producer may have lapped us (only possible when the brain thread was
    // starved for over a minute). Index i is unsafe once the producer started writing index
    // i + Capacity, i.e. when i + Capacity <= head now. Such copies are discarded, not returned.
    const std::uint64_t h2 = head.load(std::memory_order_acquire);
    if (h2 >= Capacity && h2 - Capacity >= start) {
        const std::uint64_t unsafe_end = std::min<std::uint64_t>(h2 - Capacity + 1, h);
        const std::uint64_t n = unsafe_end - start;
        out.erase(out.begin() + static_cast<std::ptrdiff_t>(first_out),
                  out.begin() + static_cast<std::ptrdiff_t>(first_out + n));
        dropped += n;
    }
    return dropped;
}

void TelemetryWindow::CloseSecond() {
    if (current_second.empty()) {
        return;
    }
    SecondSummary s;
    s.start_ns = current_second_start;
    s.frames = static_cast<std::uint32_t>(current_second.size());
    std::vector<double> ms;
    ms.reserve(current_second.size());
    for (const auto& f : current_second) {
        ms.push_back(f.frame_ms);
        s.max_ms = std::max(s.max_ms, static_cast<double>(f.frame_ms));
        for (std::size_t i = 0; i < NumStallKinds; ++i) {
            s.stall_ms[i] += f.stall_ms[i];
        }
    }
    s.median_ms = Stats::Percentile(ms, 0.5);
    s.p99_ms = Stats::Percentile(ms, 0.99);
    for (const double v : ms) {
        if (v >= HitchFloorMs && v >= 2.0 * s.median_ms) {
            ++s.hitches;
        }
    }
    seconds.push_back(s);
    while (seconds.size() > MaxSeconds) {
        seconds.pop_front();
    }
    current_second.clear();
}

void TelemetryWindow::Add(const std::vector<FrameSample>& samples) {
    constexpr std::uint64_t OneSecond = 1'000'000'000ull;
    for (const auto& f : samples) {
        ++total_frames;
        frames.push_back(f);
        if (current_second.empty()) {
            current_second_start = f.timestamp_ns;
        } else if (f.timestamp_ns >= current_second_start + OneSecond) {
            CloseSecond();
            current_second_start = f.timestamp_ns;
        }
        current_second.push_back(f);
    }
    if (!frames.empty()) {
        const std::uint64_t newest = frames.back().timestamp_ns;
        const auto keep = static_cast<std::uint64_t>(window_seconds * 1e9);
        while (!frames.empty() && newest - frames.front().timestamp_ns > keep) {
            frames.pop_front();
        }
    }
}

Snapshot TelemetryWindow::Build(std::uint64_t now_ns) const {
    Snapshot s;
    s.timestamp_ns = now_ns;
    s.resources = resources;
    s.window_frames = frames.size();
    if (frames.size() < 2) {
        return s;
    }
    s.window_seconds =
        static_cast<double>(frames.back().timestamp_ns - frames.front().timestamp_ns) / 1e9;
    std::vector<double> ms;
    ms.reserve(frames.size());
    double total_ms = 0.0;
    for (const auto& f : frames) {
        ms.push_back(f.frame_ms);
        total_ms += f.frame_ms;
        s.frame_ms_max = std::max(s.frame_ms_max, static_cast<double>(f.frame_ms));
        for (std::size_t i = 0; i < NumStallKinds; ++i) {
            s.stall_ms_per_frame[i] += f.stall_ms[i];
        }
    }
    const double n = static_cast<double>(frames.size());
    for (auto& v : s.stall_ms_per_frame) {
        v /= n;
    }
    s.fps = total_ms > 0.0 ? n * 1000.0 / total_ms : 0.0;
    s.frame_ms_p50 = Stats::Percentile(ms, 0.50);
    s.frame_ms_p95 = Stats::Percentile(ms, 0.95);
    s.frame_ms_p99 = Stats::Percentile(ms, 0.99);
    std::size_t hitches = 0;
    for (const double v : ms) {
        if (v >= HitchFloorMs && v >= 2.0 * s.frame_ms_p50) {
            ++hitches;
        }
    }
    const double minutes = total_ms / 60000.0;
    s.hitches_per_minute = minutes > 0.0 ? static_cast<double>(hitches) / minutes : 0.0;
    // Gameplay: frames keep coming (no >1 s gap) and the window is mostly filled.
    bool gap = false;
    for (std::size_t i = 1; i < frames.size(); ++i) {
        if (frames[i].timestamp_ns - frames[i - 1].timestamp_ns > 1'000'000'000ull) {
            gap = true;
            break;
        }
    }
    s.in_gameplay = !gap && s.window_seconds >= window_seconds * 0.6 && s.fps >= 5.0;
    return s;
}

std::vector<SecondSummary> TelemetryWindow::SecondsSince(std::uint64_t since_ns) const {
    std::vector<SecondSummary> out;
    for (const auto& s : seconds) {
        if (s.start_ns >= since_ns) {
            out.push_back(s);
        }
    }
    return out;
}

} // namespace Adaptive
