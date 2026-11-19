// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "adaptive/quality_ladder.h"

using namespace Adaptive;

TEST(QualityLadder, LowThenSmallerSizesThenOffice) {
    const auto l = BuildLadder({0, 2, 1080, 720});
    const std::vector<QualityStep> want = {{0, 1080}, {1, 1080}, {1, 1008}, {1, 936}, {1, 864},
                                           {1, 792},  {1, 720},  {2, 720}};
    EXPECT_EQ(l, want);
}

TEST(QualityLadder, OnlyWhatThePlayerAllows) {
    EXPECT_EQ(BuildLadder({0, 0, 1080, 1080}).size(), 1u); // nothing allowed: target off
    EXPECT_EQ(BuildLadder({0, 2, 1080, 1080}),
              (std::vector<QualityStep>{{0, 1080}, {1, 1080}, {2, 1080}})); // as before build 30
    EXPECT_EQ(BuildLadder({1, 1, 864, 720}),
              (std::vector<QualityStep>{{1, 864}, {1, 792}, {1, 720}}));
    // A smallest size above the player's own size is ignored.
    EXPECT_EQ(BuildLadder({0, 0, 792, 1080}).size(), 1u);
    // Unknown heights count as the game's own.
    EXPECT_EQ(BuildLadder({0, 0, 777, 700}).front(), (QualityStep{0, 1080}));
}

TEST(QualityLadder, FindStepExactOrClosest) {
    const auto l = BuildLadder({0, 2, 1080, 720});
    EXPECT_EQ(FindStep(l, {1, 864}), 4u);
    EXPECT_EQ(FindStep(l, {2, 720}), 7u);
    // The player raised the smallest size to 864 after the target reached 720: the closest.
    const auto narrower = BuildLadder({0, 2, 1080, 864});
    EXPECT_EQ(narrower[FindStep(narrower, {1, 720})], (QualityStep{1, 864}));
}

TEST(QualityLadder, DecideStepsDownUpOrStays) {
    const auto l = BuildLadder({0, 2, 1080, 720});
    // 30 fps target = 33.3 ms.
    EXPECT_EQ(DecideStep(l, 0, 38.0, 30.0, 30).next, 1u);  // 26 fps: one step
    EXPECT_EQ(DecideStep(l, 0, 52.0, 30.0, 30).next, 2u);  // 19 fps: two steps
    EXPECT_EQ(DecideStep(l, 7, 52.0, 30.0, 30).next, 7u);  // already the lowest allowed
    EXPECT_EQ(DecideStep(l, 3, 33.4, 30.0, 30).next, 3u);  // on target, card busy: stays
    EXPECT_EQ(DecideStep(l, 3, 33.4, 18.0, 30).next, 2u);  // on target, card idle: more quality
    EXPECT_EQ(DecideStep(l, 0, 33.4, 18.0, 30).next, 0u);  // already the best
    EXPECT_NE(DecideStep(l, 0, 38.0, 30.0, 30).reason.find("render size 1920x1080"),
              std::string::npos);
}

TEST(QualityLadder, Describe) {
    EXPECT_EQ(DescribeRenderHeight(792), "1408x792 (73%)");
    EXPECT_EQ(DescribeRenderHeight(1080), "1920x1080 (100%)");
    EXPECT_EQ(Describe({1, 720}), "performance mode low, render size 1280x720 (67%)");
}
