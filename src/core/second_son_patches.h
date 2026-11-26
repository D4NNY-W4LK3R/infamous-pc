// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// inFAMOUS Second Son (update 01.07) game patches, ported from the shadPS4-second-son v1.1.1 fork
// (src/common/memory_patcher.cpp there). They change the game's own code before its entry runs:
//
//   particle late-read  the main loop reads its particle results (survivor / light counts, CPU
//                       collision) at the start of the next frame instead of waiting for the GPU
//                       right after kicking the particle work. On by default in v1.1.1.
//   light one-submit    with late-read: the per-light ring submit becomes a NOP, so a fight's
//                       ~25-74 extra submits a frame go out with the frame's first one.
//   performance mode    the game's own quality ladder row 0 rewritten: low = LOD 0.7, one sun-shadow
//                       cascade less, particles at half resolution, 2 high-res texture blocks;
//                       office = LOD 0.5, particles at quarter resolution, no high-res texture
//                       blocks, half the pedestrians.
//   office textures     build 55: the streamer's high-res texture slots at 0 (as office) with any
//                       performance mode, e.g. low + office textures: frees video memory, softer
//                       textures up close.
//   render size         the game renders at a smaller 16:9 size (e.g. 1408x792 instead of
//                       1920x1080): the size setup's render and output sizes and the Pro / HDR
//                       mode switch's size get it; the HUD layout stays at 1920x1080 (so the HUD
//                       keeps its size) and the presenter scales the picture to the window.
//
// Every site is compared byte for byte first; a site that differs refuses that patch (all or
// nothing per patch) and the game runs unpatched. The planning is pure (unit-tested on synthetic
// images); Apply only copies the planned bytes.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Core::SecondSonPatches {

struct Options {
    bool particle_late_read = true;
    bool light_one_submit = false;
    std::uint32_t perf_mode = 0; ///< 0 off, 1 low, 2 office
    std::uint32_t render_height = 1080; ///< 1080 = the game's own; see quality_ladder.h
    bool office_textures = false; ///< no high-res texture blocks, whatever perf_mode is
};

struct Write {
    std::uint64_t offset;
    std::vector<std::uint8_t> bytes;
};

struct Plan {
    bool eboot_107 = false; ///< the anchors of update 01.07's code were found
    bool late_read = false;
    bool light_one_submit = false;
    std::uint32_t perf_mode = 0;
    bool office_textures = false; ///< no high-res texture blocks (office, or the build 55 option)
    std::uint32_t render_width = 1920, render_height = 1080; ///< the size the game renders at
    std::vector<Write> writes;
    std::vector<std::string> notes; ///< one line per decision, for the log and the F1 window
};

/// Readable(offset, size): the image range is mapped and readable (the eboot has gaps).
using ReadableFn = std::function<bool(std::uint64_t offset, std::uint64_t size)>;

/// Decides which patches apply to the eboot image at `image` (size bytes).
Plan MakePlan(const std::uint8_t* image, std::uint64_t size, const Options& options,
              const ReadableFn& readable);

/// The bytes update 01.07 holds at every site the options touch, anchors included (tests build
/// synthetic images from it).
std::vector<Write> ExpectedOriginals(const Options& options);

/// Plans and writes the patches into the loaded eboot; logs every decision. Call once, before
/// the game's entry runs.
void Apply(std::uintptr_t eboot_base, std::uint64_t eboot_size, const Options& options);

/// What Apply did this launch (for the F1 window and the session report).
const Plan& Applied();

} // namespace Core::SecondSonPatches
