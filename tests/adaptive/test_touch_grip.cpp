// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "input/touch_grip.h"

using Input::PadTouch;
using Input::TouchGrip;

TEST(TouchGrip, IdleShowsNothing) {
    TouchGrip g;
    EXPECT_EQ(g.Output(0.0), PadTouch{});
    EXPECT_FALSE(g.Busy(0.0));
}

TEST(TouchGrip, QuickClickIsAPressSeenForAFewFrames) {
    TouchGrip g;
    g.Begin(10.0);
    auto out = g.Output(10.0);
    EXPECT_TRUE(out.down[0]);
    EXPECT_TRUE(out.click);
    EXPECT_FLOAT_EQ(out.x[0], 0.5f);
    EXPECT_FLOAT_EQ(out.y[0], 0.5f);
    g.End(10.02); // released after 20 ms
    EXPECT_TRUE(g.Busy(10.05));
    EXPECT_TRUE(g.Output(10.05).down[0]);
    EXPECT_TRUE(g.Output(10.05).click);
    EXPECT_FALSE(g.Busy(10.2));
    EXPECT_EQ(g.Output(10.2), PadTouch{});
    EXPECT_FALSE(g.Last().slid);
}

TEST(TouchGrip, HoldingStillKeepsThePadPressed) {
    TouchGrip g;
    g.Begin(0.0);
    g.Move(3.0f, -2.0f); // a hand never stays perfectly still
    EXPECT_TRUE(g.Output(2.0).click);
    EXPECT_TRUE(g.Output(2.0).down[0]);
    g.End(2.0);
    EXPECT_TRUE(g.Last().pressed_at_end);
    EXPECT_EQ(g.Output(2.01), PadTouch{});
}

TEST(TouchGrip, SwipeUpSlidesTheFingerAndLiftsThePress) {
    TouchGrip g; // 600 px per pad width, pad height 0.49 of that
    g.Begin(0.0);
    for (int i = 0; i < 10; ++i) {
        g.Move(0.0f, -15.0f);
    }
    const auto out = g.Output(0.3);
    EXPECT_TRUE(out.down[0]);
    EXPECT_FALSE(out.click);
    EXPECT_FLOAT_EQ(out.x[0], 0.5f);
    EXPECT_LT(out.y[0], 0.05f); // 150 px is about half the pad's height
    g.End(0.3);
    EXPECT_TRUE(g.Last().slid);
    EXPECT_LT(g.Last().dy, -0.45f);
}

TEST(TouchGrip, SwipeAndHoldKeepsTheFingerAtTheTop) {
    TouchGrip g;
    g.Begin(0.0);
    g.Move(0.0f, -400.0f);
    for (double t = 0.5; t < 5.0; t += 0.5) {
        const auto out = g.Output(t);
        EXPECT_TRUE(out.down[0]);
        EXPECT_FLOAT_EQ(out.y[0], 0.0f); // clamped to the edge
    }
}

TEST(TouchGrip, LeftButtonPressesAgainDuringASlide) {
    TouchGrip g;
    g.Begin(0.0);
    g.Move(200.0f, 0.0f);
    EXPECT_FALSE(g.Output(0.1).click);
    g.SetPress(true);
    EXPECT_TRUE(g.Output(0.2).click);
    g.SetPress(false);
    EXPECT_FALSE(g.Output(0.3).click);
}

TEST(TouchGrip, ScannerSlideStopsWhereTheMouseStops) {
    TouchGrip g;
    g.Begin(0.0);
    g.Move(-120.0f, -90.0f); // up and to the left, towards the fingertip mark
    const auto out = g.Output(0.4);
    EXPECT_NEAR(out.x[0], 0.5f - 120.0f / 600.0f, 1e-5f);
    EXPECT_NEAR(out.y[0], 0.5f - 90.0f / (600.0f * 942.0f / 1920.0f), 1e-5f);
    EXPECT_TRUE(out.down[0]);
}

TEST(TouchGrip, RightButtonPutsAThumbOnEachHalf) {
    TouchGrip g;
    g.Begin(0.0);
    g.SetSecondFinger(true);
    auto out = g.Output(0.1);
    EXPECT_TRUE(out.down[0]);
    EXPECT_TRUE(out.down[1]);
    EXPECT_LT(out.x[0], 0.5f);
    EXPECT_GT(out.x[1], 0.5f);
    EXPECT_NEAR(out.x[1] - out.x[0], 0.4f, 1e-5f);
    // Pushed to the right edge, the pair stays on the pad and keeps its spread.
    g.Move(1000.0f, 0.0f);
    out = g.Output(0.2);
    EXPECT_FLOAT_EQ(out.x[1], 1.0f);
    EXPECT_NEAR(out.x[1] - out.x[0], 0.4f, 1e-5f);
    g.SetSecondFinger(false);
    EXPECT_FALSE(g.Output(0.3).down[1]);
    g.End(0.4);
    EXPECT_TRUE(g.Last().two_fingers);
}

TEST(TouchGrip, InputOutsideAGripIsIgnored) {
    TouchGrip g;
    g.Move(100.0f, 100.0f);
    g.SetSecondFinger(true);
    g.SetPress(true);
    g.End(1.0);
    EXPECT_EQ(g.Output(1.0), PadTouch{});
    g.Begin(2.0);
    const auto out = g.Output(2.0);
    EXPECT_FLOAT_EQ(out.x[0], 0.5f);
    EXPECT_FALSE(out.down[1]);
}

TEST(TouchGrip, EveryGripStartsInTheMiddle) {
    TouchGrip g;
    g.Begin(0.0);
    g.Move(300.0f, 100.0f);
    g.End(1.0);
    g.Begin(2.0);
    EXPECT_FLOAT_EQ(g.Output(2.0).x[0], 0.5f);
    EXPECT_FLOAT_EQ(g.Output(2.0).y[0], 0.5f);
    EXPECT_TRUE(g.Output(2.0).click);
}
