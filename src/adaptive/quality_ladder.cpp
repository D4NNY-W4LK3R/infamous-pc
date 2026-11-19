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

double EstimatedCost(const QualityStep& s) {
    static constexpr double Mode[] = {1.0, 0.8, 0.6};
    const double h = static_cast<double>(NormalizeRenderHeight(s.render_height)) / 1080.0;
    return Mode[std::min<std::uint32_t>(s.perf_mode, 2)] * (0.3 + 0.7 * h * h);
}

std::size_t FindStep(const std::vector<QualityStep>& ladder, const QualityStep& stored) {
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        if (ladder[i] == stored) {
            return i;
        }
    }
    const double want = EstimatedCost(stored);
    std::size_t best = 0;
    double best_d = 1e9;
    for (std::size_t i = 0; i < ladder.size(); ++i) {
        const double d = std::abs(EstimatedCost(ladder[i]) - want);
        if (d < best_d - 1e-9) { // ties keep the earlier, better-quality step
            best = i;
            best_d = d;
        }
    }
    return best;
}

LadderDecision DecideStep(const std::vector<QualityStep>& ladder, std::size_t current,
                          double median_ms, double gpu_busy_ms, double target_fps,
                          const LadderMemory& memory) {
    LadderDecision d;
    d.memory = memory;
    if (ladder.empty() || target_fps <= 0.0 || median_ms <= 0.0) {
        d.reason = "nothing to decide";
        return d;
    }
    current = std::min(current, ladder.size() - 1);
    d.next = current;
    const double target_ms = 1000.0 / target_fps;
    const double fps = 1000.0 / median_ms;
    const std::size_t stop_at = memory.has_stop ? FindStep(ladder, memory.stop) : ladder.size() - 1;

    // Did the last step down pay off?
    if (memory.prev_busy_ms > 0.0 && gpu_busy_ms > 0.0) {
        const std::size_t prev = FindStep(ladder, memory.prev);
        if (ladder[prev] == memory.prev && prev < current) {
            const double saved = 1.0 - gpu_busy_ms / memory.prev_busy_ms;
            d.memory.prev_busy_ms = 0.0;
            if (saved < 0.08) {
                d.next = prev;
                d.memory.has_stop = true;
                d.memory.stop = ladder[prev];
                d.reason = Fmt1("going down to this step saved only %.0f%% of the graphics "
                                "card's time (%.1f ms a frame before): ",
                                saved * 100.0, memory.prev_busy_ms) +
                           "back to " + Describe(ladder[prev]) +
                           ", and no lower again (less detail for nothing)";
                return d;
            }
        }
    }
    if (median_ms > target_ms * 1.08) {
        if (gpu_busy_ms > 0.0 && gpu_busy_ms < target_ms * 0.85) {
            d.reason = Fmt1("gameplay ran at %.1f fps, but the graphics card was busy only %.1f "
                            "ms a frame: ",
                            fps, gpu_busy_ms) +
                       "it is not the limit, so less detail would not help; it stays at " +
                       Describe(ladder[current]);
            return d;
        }
        if (current >= stop_at || current + 1 >= ladder.size()) {
            d.reason = Fmt1("gameplay ran at %.1f fps (target %.0f) at the lowest quality "
                            "that helps; ",
                            fps, target_fps) +
                       "it stays at " + Describe(ladder[current]);
            return d;
        }
        const std::size_t steps = median_ms > target_ms * 1.25 ? 2 : 1;
        d.next = std::min({current + steps, ladder.size() - 1, stop_at});
        d.memory.prev = ladder[current];
        d.memory.prev_busy_ms = gpu_busy_ms;
        d.reason = Fmt1("gameplay ran at %.1f fps (target %.0f): next launch uses ", fps,
                        target_fps) +
                   Describe(ladder[d.next]);
        return d;
    }
    d.memory.prev_busy_ms = 0.0;
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
