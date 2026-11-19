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
    // The player raised the smallest size to 864 after the target reached 720: the step with
    // the closest estimated load.
    const auto narrower = BuildLadder({0, 2, 1080, 864});
    EXPECT_EQ(narrower[FindStep(narrower, {2, 864})], (QualityStep{2, 864}));
    EXPECT_EQ(narrower[FindStep(narrower, {1, 720})], (QualityStep{2, 864}));
}

TEST(QualityLadder, OfficeAt1080IsNotTheBottomOnceSmallerSizesAreAllowed) {
    // Build 32 (a player's report): office at 1080 was stored, then the smallest size was set to
    // 1152x648 - the target jumped to office at 648, the worst picture, for no reason.
    const auto l = BuildLadder({1, 2, 1080, 648});
    const QualityStep picked = l[FindStep(l, {2, 1080})];
    EXPECT_NE(picked, (QualityStep{2, 648}));
    EXPECT_EQ(picked.perf_mode, 1u) << "a load like office at 1080 is found at low";
    EXPECT_NEAR(EstimatedCost(picked), EstimatedCost({2, 1080}), 0.05);
}

TEST(QualityLadder, NoLessDetailWhenTheCardIsNotTheLimit) {
    const auto l = BuildLadder({0, 2, 1080, 720});
    // 26 fps, but the card is busy only 20 ms of the 33.3: freezes or the CPU, not the card.
    const auto d = DecideStep(l, 1, 38.0, 20.0, 30);
    EXPECT_EQ(d.next, 1u);
    EXPECT_NE(d.reason.find("not the limit"), std::string::npos);
}

TEST(QualityLadder, AStepThatDidNotPayOffIsUndoneAndNotTakenAgain) {
    const auto l = BuildLadder({1, 2, 1080, 648});
    // Launch 1 at low 1080: 26 fps, card busy 31 ms -> down.
    auto d = DecideStep(l, 0, 38.0, 31.0, 30);
    ASSERT_EQ(d.next, 1u);
    EXPECT_DOUBLE_EQ(d.memory.prev_busy_ms, 31.0);
    // Launch 2 at low 1008: still 26 fps, card 30 ms (3% saved) -> back to low 1080 and stop.
    d = DecideStep(l, 1, 38.0, 30.0, 30, d.memory);
    EXPECT_EQ(d.next, 0u);
    EXPECT_TRUE(d.memory.has_stop);
    EXPECT_EQ(d.memory.stop, (QualityStep{1, 1080}));
    EXPECT_NE(d.reason.find("saved only 3%"), std::string::npos);
    // Launch 3 at low 1080, slow again: it stays.
    d = DecideStep(l, 0, 38.0, 31.0, 30, d.memory);
    EXPECT_EQ(d.next, 0u);
}

TEST(QualityLadder, AStepThatPaidOffIsKept) {
    const auto l = BuildLadder({1, 2, 1080, 648});
    auto d = DecideStep(l, 0, 38.0, 31.0, 30);
    ASSERT_EQ(d.next, 1u);
    // At 1008: card 26 ms (16% saved), 30 fps: kept, no stop.
    d = DecideStep(l, 1, 33.4, 26.0, 30, d.memory);
    EXPECT_EQ(d.next, 1u);
    EXPECT_FALSE(d.memory.has_stop);
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
