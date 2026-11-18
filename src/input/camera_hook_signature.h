// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Version-aware recognition of the game code the native mouse-camera hook patches.
//
// The hook replaces one store in inFAMOUS Second Son's camera update with a jump to a code cave.
// It is only correct for an executable whose code is byte-identical around that store AND whose
// data (invert options, auto-follow byte) sits at the offsets measured for it. So recognition is
// two-step:
//   1. Exact: the 27 measured bytes sit at the measured offset of a known build profile.
//      -> the hook may be installed with that profile's offsets.
//   2. Otherwise the bytes are searched for anywhere in the image. Finding them elsewhere means
//      a different build of the same code: reported (so the build can be measured and added),
//      but the hook stays OFF, because the data offsets of that build are unknown.
// There is no third option: an unrecognized executable never gets the hook, and mouse control
// falls back to the emulator's mouse-to-stick mode.
//
// No emulator dependency: unit tested on synthetic images.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Input::CameraHook {

struct BuildProfile {
    const char* name;
    std::uint64_t expected_start; ///< module offset of the first expected byte
    std::array<std::uint8_t, 27> expected;
    std::uint64_t store_offset;   ///< the 9-byte vmovups replaced by jmp + nop
    std::uint64_t invert_x_offset;
    std::uint64_t invert_y_offset;
    std::uint64_t no_follow_offset;
};

/// Builds whose offsets were measured (notes/study-input-kbm-2026-10-14.md sec. 6).
std::span<const BuildProfile> KnownBuilds();

enum class MatchKind : std::uint8_t {
    Exact,         ///< a known build: safe to hook with its offsets
    Relocated,     ///< the camera code exists at another offset: unknown build, no hook
    Ambiguous,     ///< the pattern occurs more than once elsewhere: no hook
    NotFound,      ///< not this game, or the camera code changed: no hook
    ImageTooSmall, ///< the image ends before the offsets a profile needs: no hook
};

struct MatchResult {
    MatchKind kind = MatchKind::NotFound;
    const BuildProfile* profile = nullptr; ///< Exact only
    std::uint64_t found_start = 0;         ///< Relocated: where the pattern starts
    std::size_t hits = 0;                  ///< occurrences of the pattern in the image
    std::string detail;
    bool SafeToHook() const {
        return kind == MatchKind::Exact && profile != nullptr;
    }
};

/// image: the loaded module's bytes from its base address (code and data), size = mapped size.
/// Readable ranges only: readable(offset, size) must say whether [offset, offset+size) can be
/// read; the search skips unreadable ranges (unmapped gaps between segments).
MatchResult Match(const std::uint8_t* base, std::uint64_t size,
                  bool (*readable)(const std::uint8_t* base, std::uint64_t offset,
                                   std::uint64_t size) = nullptr);

std::string ToString(MatchKind kind);

} // namespace Input::CameraHook
