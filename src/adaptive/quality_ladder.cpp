// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "adaptive/quality_ladder.h"

namespace Adaptive {

namespace {
constexpr const char* PerfNames[] = {"off", "low", "office"};

std::string Fmt1(const char* fmt, double a, double b) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), fmt, a, b);
    return buf;
}
} // namespace

std::uint32_t NormalizeRenderHeight(std::uint32_t height) {
    return std::find(RenderHeights.begin(), RenderHeights.end(), height) != RenderHeights.end()
               ? height
               : 1080u;
}

std::string DescribeRenderHeight(std::uint32_t height) {
    const std::uint32_t h = NormalizeRenderHeight(height);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%ux%u (%u%%)", RenderWidthFor(h), h,
                  static_cast<unsigned>(std::lround(100.0 * h / 1080.0)));
    return buf;
}

std::string Describe(const QualityStep& s) {
    return std::string("performance mode ") + PerfNames[std::min<std::uint32_t>(s.perf_mode, 2)] +
           ", render size " + DescribeRenderHeight(s.render_height);
}

std::vector<QualityStep> BuildLadder(const QualityLimits& in) {
    const std::uint32_t floor = std::min<std::uint32_t>(in.perf_floor, 2);
    const std::uint32_t max = std::max(floor, std::min<std::uint32_t>(in.perf_max, 2));
    const std::uint32_t own_h = NormalizeRenderHeight(in.render_height);
    // The smallest size allowed, never above the player's own.
    const std::uint32_t min_h = std::min(own_h, NormalizeRenderHeight(in.render_min));

    std::vector<QualityStep> ladder{{floor, own_h}};
    std::uint32_t perf = floor;
    if (perf < 1 && max >= 1) {
        perf = 1;
        ladder.push_back({perf, own_h});
    }
    std::uint32_t h = own_h;
    for (const std::uint32_t step : RenderHeights) {
        if (step < own_h && step >= min_h) {
            h = step;
            ladder.push_back({perf, h});
        }
    }
    if (perf < 2 && max >= 2) {
        ladder.push_back({2, h});
    }
    return ladder;
}

std::size_t FindStep(const std::vector<QualityStep>& ladder, const QualityStep& stored) {
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        if (ladder[i] == stored) {
            return i;
        }
    }
    // The limits changed since: the closest step, performance mode first, then render size.
    const auto h = static_cast<long>(NormalizeRenderHeight(stored.render_height));
    std::size_t best = 0;
    long best_perf = 1 << 20, best_h = 1 << 20;
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        const long dp = std::labs(static_cast<long>(ladder[i].perf_mode) -
                                  static_cast<long>(stored.perf_mode));
        const long dh = std::labs(static_cast<long>(ladder[i].render_height) - h);
        if (dp < best_perf || (dp == best_perf && dh < best_h)) {
            best = i;
            best_perf = dp;
            best_h = dh;
        }
    }
    return best;
}

LadderDecision DecideStep(const std::vector<QualityStep>& ladder, std::size_t current,
                          double median_ms, double gpu_busy_ms, double target_fps) {
    LadderDecision d;
    if (ladder.empty() || target_fps <= 0.0 || median_ms <= 0.0) {
        d.reason = "nothing to decide";
        return d;
    }
    current = std::min(current, ladder.size() - 1);
    d.next = current;
    const double target_ms = 1000.0 / target_fps;
    const double fps = 1000.0 / median_ms;
    if (median_ms > target_ms * 1.08) {
        if (current + 1 >= ladder.size()) {
            d.reason = Fmt1("gameplay ran at %.1f fps (target %.0f) at the lowest quality "
                            "allowed; ",
                            fps, target_fps) +
                       "it stays at " + Describe(ladder[current]);
            return d;
        }
        const std::size_t steps = median_ms > target_ms * 1.25 ? 2 : 1;
        d.next = std::min(current + steps, ladder.size() - 1);
        d.reason = Fmt1("gameplay ran at %.1f fps (target %.0f): next launch uses ", fps,
                        target_fps) +
                   Describe(ladder[d.next]);
        return d;
    }
    if (median_ms <= target_ms * 1.03 && current > 0 && gpu_busy_ms > 0.0 &&
        gpu_busy_ms < target_ms * 0.65) {
        d.next = current - 1;
        d.reason = Fmt1("gameplay held %.1f fps with the graphics card busy only %.1f ms a "
                        "frame: next launch tries ",
                        fps, gpu_busy_ms) +
                   Describe(ladder[d.next]);
        return d;
    }
    d.reason = Fmt1("gameplay ran at %.1f fps (target %.0f); it stays at ", fps, target_fps) +
               Describe(ladder[current]);
    return d;
}

} // namespace Adaptive
