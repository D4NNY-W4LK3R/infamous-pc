// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The frame-rate target's quality ladder (Architecture v1.1, build 30): the steps the optimizer
// may take, one launch at a time, to reach the player's frame rate, from the player's own choice
// down to the limits they allow. A step is the game's performance mode (its own lower quality
// levels) and the render size (the game renders fewer pixels and the presenter scales the
// picture to the window). The order trades sharpness before detail:
//
//   your choice -> performance mode low -> smaller render sizes (down to your limit) -> office
//
// so the target reaches a frame rate with low's detail at a smaller size before falling back to
// office, which takes away detail, textures and pedestrians (player request: "30 fps at better
// quality than Office").

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Adaptive {

/// Render heights the game can be given (16:9, both sides multiples of 8: width = height x 16/9).
/// 1080 is the game's own size.
inline constexpr std::array<std::uint32_t, 7> RenderHeights = {1080, 1008, 936, 864, 792, 720, 648};

constexpr std::uint32_t RenderWidthFor(std::uint32_t height) {
    return height * 16 / 9;
}
/// 1080 for 0 and for any height that is not one of RenderHeights.
std::uint32_t NormalizeRenderHeight(std::uint32_t height);
/// "1440x792 (73%)".
std::string DescribeRenderHeight(std::uint32_t height);

struct QualityStep {
    std::uint32_t perf_mode = 0;        ///< 0 off, 1 low, 2 office
    std::uint32_t render_height = 1080; ///< one of RenderHeights
    bool operator==(const QualityStep&) const = default;
};

std::string Describe(const QualityStep& s);

struct QualityLimits {
    std::uint32_t perf_floor = 0;        ///< the player's own performance mode
    std::uint32_t perf_max = 0;          ///< the lowest quality the player allows
    std::uint32_t render_height = 1080;  ///< the player's own render size
    std::uint32_t render_min = 1080;     ///< the smallest render size the player allows
};

/// The steps, best quality first. One step = nothing to choose.
std::vector<QualityStep> BuildLadder(const QualityLimits& limits);

/// Where a stored step is on the ladder: the step itself, else the closest one (performance mode
/// first, then render size) when the player's limits changed.
std::size_t FindStep(const std::vector<QualityStep>& ladder, const QualityStep& stored);

struct LadderDecision {
    std::size_t next = 0;
    std::string reason;
};

/// After a launch at ladder[current]: gameplay's median frame time and the graphics card's busy
/// time per frame (0 = unknown) against the target. Too slow (more than 8% over): down a step,
/// two when more than 25% over. On target with the card idle more than a third of the frame: up
/// one step to try more quality.
LadderDecision DecideStep(const std::vector<QualityStep>& ladder, std::size_t current,
                          double median_ms, double gpu_busy_ms, double target_fps);

} // namespace Adaptive
