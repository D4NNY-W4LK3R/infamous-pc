// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/stencil_probe.h"

using Core::StencilProbe::Distinctive;

TEST(StencilProbe, FindsAddressesSeenOnlyWithOneHold) {
    // gameplay, upright, sideways
    std::vector<std::unordered_map<u64, u32>> counts(3);
    std::vector<u32> samples{1000, 1000, 1000};
    // Engine code on the stack everywhere.
    for (auto& c : counts) {
        c[0x1000] = 900;
    }
    // The stencil update: in most sideways samples, never otherwise.
    counts[2][0x2000] = 700;
    // Spray code in both spray holds.
    counts[1][0x3000] = 600;
    counts[2][0x3000] = 650;
    // Rare noise with the sideways hold.
    counts[2][0x4000] = 50;
    // Mostly sideways but seen a little upright (5%): over the 2% allowed.
    counts[2][0x5000] = 500;
    counts[1][0x5000] = 50;
    const auto found = Distinctive(counts, samples, 2, 0.2, 0.02, 10);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].offset, 0x2000u);
    EXPECT_DOUBLE_EQ(found[0].share, 0.7);
    EXPECT_DOUBLE_EQ(found[0].other_max, 0.0);
}

TEST(StencilProbe, BucketsWithoutSamplesAreIgnored) {
    std::vector<std::unordered_map<u64, u32>> counts(3);
    std::vector<u32> samples{0, 0, 400};
    counts[2][0x10] = 300;
    const auto found = Distinctive(counts, samples, 2, 0.2, 0.02, 10);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_TRUE(Distinctive(counts, samples, 1, 0.2, 0.02, 10).empty());
}
