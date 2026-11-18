// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <random>

#include <gtest/gtest.h>

#include "adaptive/stats.h"

using namespace Adaptive::Stats;

TEST(Stats, PercentileInterpolates) {
    EXPECT_DOUBLE_EQ(Percentile({}, 0.5), 0.0);
    EXPECT_DOUBLE_EQ(Percentile({5.0}, 0.99), 5.0);
    EXPECT_DOUBLE_EQ(Percentile({1, 2, 3, 4}, 0.5), 2.5);
    EXPECT_DOUBLE_EQ(Percentile({4, 1, 3, 2}, 0.0), 1.0);
    EXPECT_DOUBLE_EQ(Percentile({4, 1, 3, 2}, 1.0), 4.0);
}

TEST(Stats, IdenticalSamplesAreNotDifferent) {
    std::vector<double> a(40, 33.3), b(40, 33.3);
    const auto c = Compare(a, b);
    EXPECT_GT(c.p_value, 0.5);
    EXPECT_DOUBLE_EQ(c.relative_change, 0.0);
}

TEST(Stats, DetectsAClearImprovement) {
    std::mt19937 rng(1);
    std::normal_distribution<double> base(33.3, 0.8), better(30.0, 0.8);
    std::vector<double> a, b;
    for (int i = 0; i < 30; ++i) {
        a.push_back(base(rng));
        b.push_back(better(rng));
    }
    const auto c = Compare(a, b);
    EXPECT_LT(c.p_value, 0.001);
    EXPECT_LT(c.relative_change, -0.05);
    EXPECT_GT(c.prob_b_lower, 0.9);
}

TEST(Stats, NoiseAloneIsRarelySignificant) {
    // Same distribution: at alpha = 0.05 about 5% of comparisons may come out significant.
    std::mt19937 rng(7);
    std::normal_distribution<double> d(33.3, 2.0);
    int significant = 0;
    for (int trial = 0; trial < 400; ++trial) {
        std::vector<double> a, b;
        for (int i = 0; i < 30; ++i) {
            a.push_back(d(rng));
            b.push_back(d(rng));
        }
        significant += Compare(a, b).p_value < 0.05 ? 1 : 0;
    }
    EXPECT_LT(significant, 40); // < 10%
}

TEST(Stats, RankTestIgnoresSpikeMagnitude) {
    std::vector<double> a(20, 33.0), b(20, 33.0);
    b[0] = 5000.0; // one absurd second must not decide the verdict
    EXPECT_GT(Compare(a, b).p_value, 0.3);
}
