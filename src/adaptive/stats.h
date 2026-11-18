// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Small statistics helpers for judging whether a change helped. Frame times are strongly
// autocorrelated, so comparisons are made on per-second summaries (one value per second), and
// the test used is rank based (Mann-Whitney U) so a few huge spikes cannot decide a verdict.

#pragma once

#include <cstddef>
#include <vector>

namespace Adaptive::Stats {

/// q in [0,1]; linear interpolation between closest ranks. Empty input returns 0.
double Percentile(std::vector<double> values, double q);
double Mean(const std::vector<double>& values);
double StdDev(const std::vector<double>& values);

struct Comparison {
    std::size_t n_a = 0;
    std::size_t n_b = 0;
    double median_a = 0.0;
    double median_b = 0.0;
    /// (median_b - median_a) / median_a; negative = B has lower values.
    double relative_change = 0.0;
    /// Two-sided p-value of the Mann-Whitney U test (normal approximation with tie correction).
    double p_value = 1.0;
    /// Probability that a random B value is lower than a random A value (common-language effect
    /// size); 0.5 = no difference.
    double prob_b_lower = 0.5;
};

/// Compares two samples where lower is better (frame times).
Comparison Compare(const std::vector<double>& a, const std::vector<double>& b);

} // namespace Adaptive::Stats
