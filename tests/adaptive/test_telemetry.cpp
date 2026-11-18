// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdio>
#include <thread>

#include <gtest/gtest.h>

#include "adaptive/telemetry.h"

using namespace Adaptive;

namespace {
FrameSample Frame(std::uint64_t i, std::uint64_t t_ns, float ms) {
    FrameSample f;
    f.frame_index = i;
    f.timestamp_ns = t_ns;
    f.frame_ms = ms;
    return f;
}
} // namespace

TEST(FrameRing, DrainsInOrder) {
    FrameRing ring;
    for (std::uint64_t i = 0; i < 100; ++i) {
        ring.Push(Frame(i, i * 1000, 16.0f));
    }
    std::vector<FrameSample> out;
    EXPECT_EQ(ring.Drain(out), 0u);
    ASSERT_EQ(out.size(), 100u);
    for (std::uint64_t i = 0; i < 100; ++i) {
        EXPECT_EQ(out[i].frame_index, i);
    }
    out.clear();
    EXPECT_EQ(ring.Drain(out), 0u);
    EXPECT_TRUE(out.empty());
}

TEST(FrameRing, OverflowDropsOldestAndCountsThem) {
    FrameRing ring;
    const std::uint64_t n = FrameRing::Capacity + 500;
    for (std::uint64_t i = 0; i < n; ++i) {
        ring.Push(Frame(i, i, 16.0f));
    }
    std::vector<FrameSample> out;
    // With the ring exactly full, the oldest slot is the one a producer would write next; the
    // consumer cannot tell whether a write is in flight, so it discards that one sample too.
    EXPECT_EQ(ring.Drain(out), 501u);
    ASSERT_EQ(out.size(), FrameRing::Capacity - 1);
    EXPECT_EQ(out.front().frame_index, 501u);
    EXPECT_EQ(out.back().frame_index, n - 1);
}

TEST(FrameRing, ConcurrentProducerNeverYieldsTornOrDuplicateFrames) {
    FrameRing ring;
    constexpr std::uint64_t N = 200000;
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < N; ++i) {
            FrameSample f = Frame(i, i, static_cast<float>(i % 1000));
            f.draws = static_cast<std::uint32_t>(i); // checked against frame_index
            ring.Push(f);
        }
    });
    std::vector<FrameSample> out;
    std::uint64_t dropped = 0;
    std::uint64_t last = 0;
    bool first = true;
    while (true) {
        out.clear();
        dropped += ring.Drain(out);
        for (const auto& f : out) {
            ASSERT_EQ(f.draws, static_cast<std::uint32_t>(f.frame_index)) << "torn sample";
            if (!first) {
                ASSERT_GT(f.frame_index, last) << "out of order / duplicate";
            }
            first = false;
            last = f.frame_index;
        }
        if (ring.Published() == N && out.empty()) {
            break;
        }
    }
    producer.join();
    out.clear();
    dropped += ring.Drain(out);
    for (const auto& f : out) {
        ASSERT_EQ(f.draws, static_cast<std::uint32_t>(f.frame_index));
        ASSERT_TRUE(first || f.frame_index > last);
        first = false;
        last = f.frame_index;
    }
    EXPECT_EQ(last, N - 1);
}

TEST(TelemetryWindow, SnapshotStatistics) {
    TelemetryWindow w(10.0);
    std::vector<FrameSample> frames;
    std::uint64_t t = 1'000'000'000;
    for (int i = 0; i < 300; ++i) { // 10 s at 30 fps, one 100 ms hitch
        const float ms = i == 150 ? 100.0f : 33.3f;
        t += static_cast<std::uint64_t>(ms * 1e6);
        FrameSample f = Frame(i, t, ms);
        f.stall_ms[static_cast<std::size_t>(StallKind::GuestWait)] = 12.0f;
        frames.push_back(f);
    }
    w.Add(frames);
    const Snapshot s = w.Build(t);
    EXPECT_NEAR(s.frame_ms_p50, 33.3, 0.01);
    EXPECT_NEAR(s.frame_ms_max, 100.0, 0.01);
    EXPECT_GT(s.hitches_per_minute, 4.0);
    EXPECT_NEAR(s.stall_ms_per_frame[static_cast<std::size_t>(StallKind::GuestWait)], 12.0, 0.01);
    EXPECT_TRUE(s.in_gameplay);
    EXPECT_NEAR(s.fps, 29.8, 0.5);
    EXPECT_GE(w.Seconds().size(), 9u);
}

TEST(TelemetryWindow, LoadingGapIsNotGameplay) {
    TelemetryWindow w(10.0);
    std::vector<FrameSample> frames;
    std::uint64_t t = 0;
    for (int i = 0; i < 300; ++i) {
        t += i == 100 ? 3'000'000'000ull : 33'000'000ull; // a 3 s load screen
        frames.push_back(Frame(i, t, 33.0f));
    }
    w.Add(frames);
    EXPECT_FALSE(w.Build(t).in_gameplay);
}

TEST(FrameRing, PublishCostIsSmall) {
    // The flip thread pays this once per frame. Reported, and bounded generously so sanitizer
    // and debug builds pass; a release build measures tens of nanoseconds.
    FrameRing ring;
    FrameSample f = Frame(0, 0, 16.6f);
    constexpr int N = 200000;
    std::vector<FrameSample> sink;
    sink.reserve(FrameRing::Capacity);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < N; ++i) {
        f.frame_index = static_cast<std::uint64_t>(i);
        ring.Push(f);
        if ((i & 2047) == 2047) {
            sink.clear();
            ring.Drain(sink);
        }
    }
    const double ns =
        std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / N;
    std::printf("[ measure  ] FrameRing::Push + amortized Drain: %.1f ns per frame\n", ns);
    EXPECT_LT(ns, 5000.0);
}
