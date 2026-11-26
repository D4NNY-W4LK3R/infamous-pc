// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// User configuration of the optimizer: <user dir>/adaptive/settings.ini, plain NAME=VALUE lines.
//
//   mode = automatic       observe | manual | automatic | calibration (automatic: the player's
//                          choice of default; every change is still measured and reverted if worse)
//   enabled = 1            0 turns the whole optimizer off (no thread, no telemetry)
//   overlay = 1            the F1 optimizer window is available
//   controls_hint = 1      on-screen keyboard / mouse equivalents of the button prompts (Home)
//   calibrate_on_battery = 0
//   tick_ms = 1000
//   frame_limit = 0        most frames per second shown (0 = no limit; F3 > Graphics)
//   perf_mode = off        Second Son's own quality levels: off | low | office (next launch)
//   particle_late_read = 0 Second Son reads particle results a frame later instead of waiting
//                          for the GPU mid-frame (next launch; game update 01.07 only). Off by
//                          default: with it on, players saw white flashes and the emulator
//                          stopped on a page-protection assertion.
//   particle_light_one_submit = 0  experimental, needs particle_late_read (next launch)
//   fps_target = 0         the optimizer lowers the game's quality level (per launch) to reach
//                          this frame rate; 0 = off
//   auto_perf_max = off    the lowest quality it may use: off (none) | low | office
//   empty_memory_out = 1   leave empty buffer memory out of video memory (next launch;
//                          SS_LAZY_ZERO_MEMORY). On by default since build 51; the older key
//                          lazy_zero_memory is read no more, so every setup starts with it on.
//   early_copy_back = 0    experiment: copy back what game threads read right after the GPU
//                          wrote it (next launch; SS_EARLY_COPY_BACK)
//   keep_rewritten_pages = 0  experiment: pages written every frame stay unprotected (next
//                          launch; SS_KEEP_REWRITTEN_PAGES)
//   vram_priorities = 0    memory priorities for the driver: buffers highest, images drawn to
//                          next, textures last (next launch; SS_VRAM_PRIORITIES)
//
// Environment variables win over the file (SS_ADAPTIVE_MODE, SS_ADAPTIVE=0). The overlay writes
// the file back when the user changes the mode, so the choice is remembered.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "adaptive/contracts.h"

namespace Adaptive {

struct UserSettings {
    bool enabled = true;
    Mode mode = Mode::Automatic;
    bool overlay = true;
    bool controls_hint = true; ///< keyboard / mouse equivalents of the button prompts (Home)
    bool calibrate_on_battery = false;
    std::uint32_t tick_ms = 1000;
    std::uint32_t frame_limit = 0; ///< frames per second shown at most; 0 = no limit
    std::uint32_t perf_mode = 0;   ///< Second Son quality preset: 0 off, 1 low, 2 office
    bool particle_late_read = false; // off by default since build 25: flashes and crashes
    bool particle_light_one_submit = false;
    std::uint32_t fps_target = 0;    ///< 0 = off
    std::uint32_t auto_perf_max = 0; ///< 0 = the optimizer may not lower quality
    /// The game's render size: height of one of quality_ladder.h's RenderHeights (1080 = its own;
    /// smaller = fewer pixels, the presenter scales the picture to the window). Next launch.
    std::uint32_t render_height = 1080;
    /// The smallest render size the frame-rate target may use (1080 = it never changes the size).
    std::uint32_t auto_render_min = 1080;
    /// VRAM priorities (build 41, next launch): buffers' memory highest, images drawn to higher
    /// than textures, so the driver moves textures out first on a card short of memory.
    bool vram_priorities = false;
    /// Empty buffer memory left out of video memory (build 44, next launch): blocks of the large
    /// ranges the game reads that hold only zeros are not made resident until data lands there.
    bool lazy_zero_memory = true;
    /// Experiments (build 51, next launch, off by default): copy back the memory game threads
    /// read back right after the GPU work that writes it; keep pages the game writes again every
    /// frame unprotected and upload them every frame.
    bool early_copy_back = false;
    bool keep_rewritten_pages = false;
    std::vector<std::string> warnings; ///< unknown keys, bad values (not fatal)
};

UserSettings ParseSettings(const std::string& text);
std::string SerializeSettings(const UserSettings& s);

/// Reads the file (a missing file gives defaults), then applies environment overrides.
UserSettings LoadSettings(const std::filesystem::path& file,
                          const std::function<std::optional<std::string>(const std::string&)>& env);
bool SaveSettings(const std::filesystem::path& file, const UserSettings& s);

} // namespace Adaptive
