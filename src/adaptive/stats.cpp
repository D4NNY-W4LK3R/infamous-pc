// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

#include "adaptive/stats.h"

namespace Adaptive::Stats {

double Percentile(std::vector<double> values, double q) {
    if (values.empty()) {
        return 0.0;
    }
    q = std::clamp(q, 0.0, 1.0);
    std::sort(values.begin(), values.end());
    const double pos = q * static_cast<double>(values.size() - 1);
    const auto lo = static_cast<std::size_t>(std::floor(pos));
    const auto hi = std::min(lo + 1, values.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return values[lo] + (values[hi] - values[lo]) * frac;
}

double Mean(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

double StdDev(const std::vector<double>& values) {
    if (values.size() < 2) {
        return 0.0;
    }
    const double m = Mean(values);
    double acc = 0.0;
    for (const double v : values) {
        acc += (v - m) * (v - m);
    }
    return std::sqrt(acc / static_cast<double>(values.size() - 1));
}

Comparison Compare(const std::vector<double>& a, const std::vector<double>& b) {
    Comparison c;
    c.n_a = a.size();
    c.n_b = b.size();
    c.median_a = Percentile(a, 0.5);
    c.median_b = Percentile(b, 0.5);
    if (c.median_a > 0.0) {
        c.relative_change = (c.median_b - c.median_a) / c.median_a;
    }
    if (a.empty() || b.empty()) {
        return c;
    }
    // Rank all values together; ties get their average rank.
    std::vector<std::pair<double, int>> all;
    all.reserve(a.size() + b.size());
    for (const double v : a) {
        all.emplace_back(v, 0);
    }
    for (const double v : b) {
        all.emplace_back(v, 1);
    }
    std::sort(all.begin(), all.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
    const double n = static_cast<double>(all.size());
    double rank_sum_b = 0.0;
    double tie_term = 0.0;
    for (std::size_t i = 0; i < all.size();) {
        std::size_t j = i;
        while (j < all.size() && all[j].first == all[i].first) {
            ++j;
        }
        const double avg_rank = (static_cast<double>(i + 1) + static_cast<double>(j)) / 2.0;
        const double t = static_cast<double>(j - i);
        tie_term += t * t * t - t;
        for (std::size_t k = i; k < j; ++k) {
            if (all[k].second == 1) {
                rank_sum_b += avg_rank;
            }
        }
        i = j;
    }
    const double na = static_cast<double>(a.size());
    const double nb = static_cast<double>(b.size());
    const double u_b = rank_sum_b - nb * (nb + 1.0) / 2.0; // pairs where b > a (+ ties / 2)
    // u_b counts B values ranked above A values; lower is better, so "B lower" is the rest.
    c.prob_b_lower = 1.0 - u_b / (na * nb);
    const double mean_u = na * nb / 2.0;
    const double var_u = na * nb / 12.0 * ((n + 1.0) - tie_term / (n * (n - 1.0)));
    if (var_u <= 0.0) {
        c.p_value = 1.0;
        return c;
    }
    // Continuity correction.
    const double z = (std::abs(u_b - mean_u) - 0.5) / std::sqrt(var_u);
    c.p_value = std::clamp(std::erfc(std::max(z, 0.0) / std::sqrt(2.0)), 0.0, 1.0);
    return c;
}

} // namespace Adaptive::Stats
