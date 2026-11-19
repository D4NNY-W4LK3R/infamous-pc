// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstring>

#include "core/second_son_patches.h"

#ifndef SECOND_SON_PATCHES_STANDALONE
#include "common/logging/log.h"
#ifdef _WIN32
#include <windows.h>
#endif
#endif

namespace Core::SecondSonPatches {

namespace {

using Bytes = std::vector<std::uint8_t>;

struct Site {
    std::uint64_t offset;
    Bytes original;
    Bytes patched; ///< empty: the bytes are only checked, not written
};

std::string Hex(const std::uint8_t* p, std::size_t n) {
    std::string s;
    char b[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(b, sizeof(b), "%02x", p[i]);
        s += b;
    }
    return s;
}

/// Every site matches: then (and only then) its writes are added to the plan.
bool CheckSites(const std::uint8_t* image, std::uint64_t size, const ReadableFn& readable,
                const std::vector<Site>& sites, const char* what, Plan& plan) {
    for (const auto& s : sites) {
        char off[32];
        std::snprintf(off, sizeof(off), "%llx", static_cast<unsigned long long>(s.offset));
        if (s.offset + s.original.size() > size || !readable(s.offset, s.original.size())) {
            plan.notes.push_back(std::string(what) + ": refused, eboot+0x" + off +
                                 " is not loaded");
            return false;
        }
        if (std::memcmp(image + s.offset, s.original.data(), s.original.size()) != 0) {
            plan.notes.push_back(std::string(what) + ": refused, eboot+0x" + off + " holds " +
                                 Hex(image + s.offset, s.original.size()) +
                                 " (not update 01.07's code)");
            return false;
        }
    }
    for (const auto& s : sites) {
        if (!s.patched.empty()) {
            plan.writes.push_back({s.offset, s.patched});
        }
    }
    return true;
}

// Update 01.07 anchors (v1.1.1 DetectEboot107): the light list's imul esi, r15d, 0x320000 and the
// projection builder's 2.0 load, plus the highest data offset the patches rely on.
constexpr std::uint64_t ImulOffset = 0x16be04;
constexpr std::uint64_t TwoLoadOffset = 0x581d10;
constexpr std::uint64_t HighestData = 0x7f2d2b8;

bool Eboot107(const std::uint8_t* image, std::uint64_t size, const ReadableFn& readable) {
    static constexpr std::uint8_t imul[7] = {0x41, 0x69, 0xf7, 0x00, 0x00, 0x32, 0x00};
    static constexpr std::uint8_t two_load[8] = {0xc5, 0xfa, 0x10, 0x1d, 0x98, 0x27, 0x56, 0x00};
    if (size < HighestData + 8 || !readable(ImulOffset, sizeof(imul)) ||
        !readable(TwoLoadOffset, sizeof(two_load))) {
        return false;
    }
    return std::memcmp(image + ImulOffset, imul, sizeof(imul)) == 0 &&
           std::memcmp(image + TwoLoadOffset, two_load, sizeof(two_load)) == 0;
}

// Particle late-read (v1.1.1 SHADPS4_PARTICLE_LATE_READ, "V1"): a cave in the unreferenced function
// at 0x3ee160 calls 0x3edef0 (readback) and 0x3ee470 (collision join + label wait), sets up
// 0x3ee7d0's arguments and jumps back; the main loop jumps to it at 0x29cc89; the old 0x3edef0
// call at 0x29cd1e becomes a 5-byte NOP. The end-of-frame 0x3ee470 call at 0x29cd56 stays.
std::vector<Site> LateReadSites() {
    return {
        // FUN_8003ee160 (no references): 43 code bytes + 5 pad -> the cave
        {0x3ee160,
         {0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x8b, 0x5f, 0x58, 0xeb, 0x13,
          0x0f, 0x1f, 0x40, 0x00, 0x48, 0x89, 0xdf, 0xe8, 0x58, 0x5d, 0x00, 0x00,
          0x48, 0x8b, 0x9b, 0xb0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xdb, 0x75, 0xec,
          0x48, 0x83, 0xc4, 0x08, 0x5b, 0x5d, 0xc3, 0x90, 0x90, 0x90, 0x90, 0x90},
         // lea rbx,[0x807d3eb10]; mov rdi,rbx; call 0x3edef0; mov rdi,rbx; call 0x3ee470;
         // lea rax,[0x800f1bcd0]; mov rdi,rbx; mov rsi,[rax+0x1dca0]; jmp 0x29cc9e
         {0x48, 0x8d, 0x1d, 0xa9, 0x09, 0x95, 0x07, 0x48, 0x89, 0xdf, 0xe8, 0x81,
          0xfd, 0xff, 0xff, 0x48, 0x89, 0xdf, 0xe8, 0xf9, 0x02, 0x00, 0x00, 0x48,
          0x8d, 0x05, 0x52, 0xdb, 0xb2, 0x00, 0x48, 0x89, 0xdf, 0x48, 0x8b, 0xb0,
          0xa0, 0xdc, 0x01, 0x00, 0xe9, 0x11, 0xeb, 0xea, 0xff}},
        // call 0x41b8d0; lea rax,[0x800f1bcd0]; lea rdi,[0x807d3eb10]; mov rsi,[rax+0x1dca0];
        // call 0x3ee7d0 (checked); from 0x29cc89: jmp 0x3ee160 + 16 NOPs up to the call at 0x29cc9e
        {0x29cc84,
         {0xe8, 0x47, 0xec, 0x17, 0x00, 0x48, 0x8d, 0x05, 0x40, 0xf0, 0xc7,
          0x00, 0x48, 0x8d, 0x3d, 0x79, 0x1e, 0xaa, 0x07, 0x48, 0x8b, 0xb0,
          0xa0, 0xdc, 0x01, 0x00, 0xe8, 0x2d, 0x1b, 0x15, 0x00},
         {}},
        {0x29cc89,
         {0x48, 0x8d, 0x05, 0x40, 0xf0, 0xc7, 0x00, 0x48, 0x8d, 0x3d, 0x79,
          0x1e, 0xaa, 0x07, 0x48, 0x8b, 0xb0, 0xa0, 0xdc, 0x01, 0x00},
         {0xe9, 0xd2, 0x14, 0x15, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
          0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90}},
        // lea rbx,[0x807d3eb10]; mov rdi,rbx; call 0x3edef0 -> the call becomes nopl 0(rax,rax,1)
        {0x29cd14,
         {0x48, 0x8d, 0x1d, 0xf5, 0x1d, 0xaa, 0x07, 0x48, 0x89, 0xdf, 0xe8, 0xcd, 0x11, 0x15, 0x00},
         {}},
        {0x29cd1e, {0xe8, 0xcd, 0x11, 0x15, 0x00}, {0x0f, 0x1f, 0x44, 0x00, 0x00}},
        // mov rdi,rbx; call 0x3ee470 (kept: the end-of-frame join)
        {0x29cd53, {0x48, 0x89, 0xdf, 0xe8, 0x15, 0x17, 0x15, 0x00}, {}},
    };
}

// Light one-submit (v1.1.1 SHADPS4_PARTICLE_LIGHT_ONE_SUBMIT): call 0x1691e0 at 0x3f4036 -> NOP.
std::vector<Site> LightOneSubmitSites() {
    return {{0x3f4036, {0xe8, 0xa5, 0x51, 0xd7, 0xff}, {0x0f, 0x1f, 0x44, 0x00, 0x00}}};
}

// Performance mode (v1.1.1 SHADPS4_PERF_MODE): the game's 21-row quality ladder at 0xda08c0
// (20 bytes a row: LOD scale, sun-shadow cascade offset, ...) - row 0, used in normal play, gets
// the preset; the low-res particle level is always set; the streamer's high-res texture slots
// (4 nearest blocks) are reduced; office also halves the pedestrian ratios.
std::vector<Site> PerfModeSites(std::uint32_t mode) {
    const Bytes row0 = {0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x24,
                        0x74, 0x49, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    Bytes row = row0;
    const float lod = mode == 1 ? 0.7f : 0.5f;
    std::memcpy(row.data(), &lod, 4);
    const std::int32_t cascades = -1;
    std::memcpy(row.data() + 4, &cascades, 4);
    const std::uint8_t particle_level = mode == 1 ? 1 : 2; // 1.0 / 0.5 / 0.25 resolution
    const std::uint8_t hires_slots = mode == 1 ? 2 : 0;
    std::vector<Site> sites = {
        {0xda08c0, row0, row},
        // FUN_8003ed6a0: je over the "set level" store -> 2 NOPs, and the stored level
        {0x3ed6b0, {0x74, 0x0f}, {0x90, 0x90}},
        {0x3ed6b8, {0x00, 0x00, 0x00, 0x00}, {particle_level, 0x00, 0x00, 0x00}},
        // FUN_8000fd890: mov r12d, 4 / cmp eax, 4 (the 4 nearest blocks get .hires.xpps)
        {0xfe9a2, {0x04}, {hires_slots}},
        {0xfe9b2, {0x04}, {hires_slots}},
    };
    if (mode == 2) {
        // pedestrian count per class = level count x ratio[world state]: 1.0 -> 0.5
        sites.push_back({0x40ca01, {0x00, 0x00, 0x80, 0x3f}, {0x00, 0x00, 0x00, 0x3f}});
        sites.push_back({0x40c83f, {0x00, 0x00, 0x80, 0x3f}, {0x00, 0x00, 0x00, 0x3f}});
    }
    return sites;
}

// Render size (v1.1.1 SHADPS4_RENDER_HEIGHT, without its live changes): the renderer's size setup
// FUN_8001eff00 stores the render size and the output size (the video-out buffers) from four
// imm32 1920 / 1080; the Pro / HDR mode switch FUN_8001f0be0 re-applies them from two more. All
// six get the new size. The 2D screen space (HUD layout, the camera's aspect) at 0x1f0403 /
// 0x1f03fb is only checked: it stays 1920 x 1080, so the HUD keeps its size at any render size
// (v1.1.1 s1440: a 1280x720 screen space drew the minimap 1.2x too large).
bool IsRenderHeight(std::uint32_t h) {
    for (const std::uint32_t v : {1080u, 1008u, 936u, 864u, 792u, 720u, 648u}) {
        if (v == h) {
            return true;
        }
    }
    return false;
}

Bytes Le32(std::uint32_t v) {
    return {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
            static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
}

std::vector<Site> RenderSizeSites(std::uint32_t height) {
    const std::uint32_t width = height * 16 / 9;
    const Bytes w0 = Le32(1920), h0 = Le32(1080);
    const Bytes w = Le32(width), h = Le32(height);
    return {
        {0x1eff4a, w0, w}, {0x1eff51, h0, h}, // render size
        {0x1eff57, w0, w}, {0x1eff5e, h0, h}, // output size (video-out buffers)
        {0x1f0d3e, w0, w}, {0x1f0d4d, h0, h}, // the mode switch's size
        {0x1f0403, w0, {}}, {0x1f03fb, h0, {}}, // 2D screen space: checked, kept
    };
}

Plan g_applied;

} // namespace

std::vector<Write> ExpectedOriginals(const Options& options) {
    std::vector<Write> out = {
        {ImulOffset, {0x41, 0x69, 0xf7, 0x00, 0x00, 0x32, 0x00}},
        {TwoLoadOffset, {0xc5, 0xfa, 0x10, 0x1d, 0x98, 0x27, 0x56, 0x00}},
    };
    const auto add = [&out](const std::vector<Site>& sites) {
        for (const auto& s : sites) {
            out.push_back({s.offset, s.original});
        }
    };
    if (options.particle_late_read) {
        add(LateReadSites());
    }
    if (options.light_one_submit) {
        add(LightOneSubmitSites());
    }
    if (options.perf_mode == 1 || options.perf_mode == 2) {
        add(PerfModeSites(options.perf_mode));
    }
    if (options.render_height != 1080 && IsRenderHeight(options.render_height)) {
        add(RenderSizeSites(options.render_height));
    }
    return out;
}

Plan MakePlan(const std::uint8_t* image, std::uint64_t size, const Options& options,
              const ReadableFn& readable) {
    Plan plan;
    const bool resize = options.render_height != 1080;
    if (!options.particle_late_read && !options.light_one_submit && options.perf_mode == 0 &&
        !resize) {
        plan.notes.push_back("Second Son game patches: all turned off");
        return plan;
    }
    plan.eboot_107 = Eboot107(image, size, readable);
    if (!plan.eboot_107) {
        plan.notes.push_back("Second Son game patches: off - this game code is not update 01.07");
        return plan;
    }
    if (options.particle_late_read) {
        plan.late_read =
            CheckSites(image, size, readable, LateReadSites(), "particle late-read", plan);
        if (plan.late_read) {
            plan.notes.push_back("particle late-read: on (particle results read one frame later, "
                                 "no mid-frame wait for the GPU)");
        }
    }
    if (options.light_one_submit) {
        if (!plan.late_read) {
            plan.notes.push_back("particle lights in one submit: refused, needs particle late-read");
        } else {
            plan.light_one_submit = CheckSites(image, size, readable, LightOneSubmitSites(),
                                               "particle lights in one submit", plan);
            if (plan.light_one_submit) {
                plan.notes.push_back("particle lights in one submit: on");
            }
        }
    }
    if (options.perf_mode == 1 || options.perf_mode == 2) {
        const char* name = options.perf_mode == 1 ? "low" : "office";
        if (CheckSites(image, size, readable, PerfModeSites(options.perf_mode),
                       "performance mode", plan)) {
            plan.perf_mode = options.perf_mode;
            plan.notes.push_back(
                std::string("performance mode ") + name +
                (options.perf_mode == 1
                     ? ": LOD 0.7, one sun-shadow cascade less, particles at half resolution, "
                       "2 high-res texture blocks"
                     : ": LOD 0.5, one sun-shadow cascade less, particles at quarter resolution, "
                       "no high-res texture blocks, half the pedestrians"));
        }
    }
    if (resize) {
        if (!IsRenderHeight(options.render_height)) {
            plan.notes.push_back("render size: refused, " + std::to_string(options.render_height) +
                                 " lines is not one of 1080 1008 936 864 792 720 648");
        } else if (CheckSites(image, size, readable, RenderSizeSites(options.render_height),
                              "render size", plan)) {
            plan.render_height = options.render_height;
            plan.render_width = options.render_height * 16 / 9;
            plan.notes.push_back("render size: " + std::to_string(plan.render_width) + "x" +
                                 std::to_string(plan.render_height) +
                                 " (the picture is scaled to the window; the HUD keeps its size)");
        }
    }
    return plan;
}

#ifndef SECOND_SON_PATCHES_STANDALONE
void Apply(std::uintptr_t eboot_base, std::uint64_t eboot_size, const Options& options) {
    const auto* image = reinterpret_cast<const std::uint8_t*>(eboot_base);
    const ReadableFn readable = [image](std::uint64_t offset, std::uint64_t size) {
#ifdef _WIN32
        // Committed, readable memory only: the eboot image has unmapped gaps between segments.
        const std::uint8_t* p = image + offset;
        const std::uint8_t* end = p + size;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
                (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
                return false;
            }
            p = static_cast<const std::uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        }
        return true;
#else
        (void)offset;
        (void)size;
        return true;
#endif
    };
    Plan plan = MakePlan(image, eboot_size, options, readable);
    for (const auto& w : plan.writes) {
        std::memcpy(reinterpret_cast<void*>(eboot_base + w.offset), w.bytes.data(),
                    w.bytes.size());
    }
    for (const auto& n : plan.notes) {
        LOG_INFO(Loader, "Second Son: {}", n);
    }
    g_applied = std::move(plan);
}
#endif

const Plan& Applied() {
    return g_applied;
}

} // namespace Core::SecondSonPatches
