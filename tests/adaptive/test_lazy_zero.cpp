// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <set>

#include <gtest/gtest.h>

#include "video_core/buffer_cache/lazy_zero.h"

using namespace VideoCore::LazyZero;

namespace {
IntervalList<> List(std::initializer_list<std::pair<u64, u64>> ranges) {
    IntervalList<> out;
    for (const auto& [a, b] : ranges) {
        out.Add({a, b});
    }
    return out;
}
std::vector<std::pair<u64, u64>> Pairs(const IntervalList<>& list) {
    std::vector<std::pair<u64, u64>> out;
    for (const auto& r : list) {
        out.emplace_back(r.start, r.end);
    }
    return out;
}
} // namespace

TEST(LazyZero, SubtractCutsHolesAndEnds) {
    const auto from = List({{0, 10}, {20, 30}, {40, 50}});
    const auto removed = List({{5, 22}, {25, 26}, {45, 60}});
    EXPECT_EQ(Pairs(Subtract(from, removed)),
              (std::vector<std::pair<u64, u64>>{{0, 5}, {22, 25}, {26, 30}, {40, 45}}));
    EXPECT_EQ(Pairs(Subtract(from, IntervalList<>{})), Pairs(from));
    EXPECT_TRUE(Subtract(from, List({{0, 100}})).Empty());
}

TEST(LazyZero, SplitLeavesZerosOutAndWakesBlocksWithData) {
    constexpr u32 shift = 4; // 16-byte blocks
    // Blocks 2..6 are taken as zeros; the guest has data only in block 4.
    const auto lazy = List({{2, 6}});
    const auto is_zero = [](u64 addr, u64 size) { return addr + size <= 64 || addr >= 80; };
    // One copy over blocks 1..7 (bytes 16..112).
    const auto split = SplitUploads(std::vector<Piece>{{16, 96}}, lazy, shift, is_zero);
    // Block 1 and blocks 6 on are not taken as zeros: copied as they were.
    EXPECT_EQ(split.kept, (std::vector<Piece>{{16, 16}, {96, 16}}));
    EXPECT_EQ(Pairs(split.wake), (std::vector<std::pair<u64, u64>>{{4, 5}}));
    EXPECT_EQ(split.dropped, 48u); // blocks 2, 3 and 5
}

TEST(LazyZero, SplitOfAPartialCopyWakesTheWholeBlock) {
    constexpr u32 shift = 4;
    const auto lazy = List({{0, 4}});
    const auto split = SplitUploads(std::vector<Piece>{{20, 4}}, lazy, shift,
                                    [](u64, u64) { return false; });
    EXPECT_TRUE(split.kept.empty());
    EXPECT_EQ(Pairs(split.wake), (std::vector<std::pair<u64, u64>>{{1, 2}}));
    EXPECT_EQ(split.dropped, 0u);
}

TEST(LazyZero, SplitWithoutLazyBlocksKeepsAndMergesCopies) {
    const auto split = SplitUploads(std::vector<Piece>{{0, 16}, {16, 16}, {64, 8}},
                                    IntervalList<>{}, 4, [](u64, u64) { return true; });
    EXPECT_EQ(split.kept, (std::vector<Piece>{{0, 32}, {64, 8}}));
    EXPECT_TRUE(split.wake.Empty());
}
