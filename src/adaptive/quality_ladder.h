// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The frame-rate target's quality ladder (Architecture v1.1, build 30): the steps the analyser
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

/// A rough share of the graphics card's work for a step (1 = full quality at 1080): the
/// performance modes save about 20% / 40%, and only part of the work follows the pixel count.
double EstimatedCost(const QualityStep& s);

/// Where a stored step is on the ladder: the step itself, else the one with the closest
/// estimated cost (the better quality when two are as close) - when the player's limits changed,
/// e.g. office at 1080 becomes low at 936 once smaller sizes are allowed, not office at 648.
std::size_t FindStep(const std::vector<QualityStep>& ladder, const QualityStep& stored);

/// What the target remembers between launches (kept on the knowledge file's autoperf line).
struct LadderMemory {
    /// The step before the last step down and the card's busy time there (ms a frame; 0 = none),
    /// to see whether the step down paid off.
    double prev_busy_ms = 0.0;
    QualityStep prev{};
    /// A step down from here did not pay off: the target does not go below it again.
    bool has_stop = false;
    QualityStep stop{};
};

struct LadderDecision {
    std::size_t next = 0;
    std::string reason;
    LadderMemory memory;
};

/// After a launch at ladder[current]: gameplay's median frame time and the graphics card's busy
/// time per frame (0 = unknown) against the target.
///  - The last step down saved less than 8% of the card's time: back up, and no lower again
///    (less detail for nothing - a player's report, build 32: 64% fewer pixels saved 20%).
///  - Too slow (more than 8% over) while the card is the limit (busy at least 85% of the target
///    frame): down a step, two when more than 25% over. Slow with the card not the limit: it
///    stays - the time goes elsewhere (freezes, the CPU) and less detail would not help.
///  - On target with the card idle more than a third of the frame: up one step.
LadderDecision DecideStep(const std::vector<QualityStep>& ladder, std::size_t current,
                          double median_ms, double gpu_busy_ms, double target_fps,
                          const LadderMemory& memory = {});

} // namespace Adaptive
