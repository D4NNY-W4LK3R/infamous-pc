// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// inFAMOUS Second Son cheats (build 52), switched on and off at once from the F1 window's
// Cheats section. Only "infinite" cheats and the dash cheats: no god mode, no karma level, no
// hotkeys.
//
//   from the GoldHEN cheat database, json/CUSA00004_01.07.json and _2.json (Weysincha, 2B):
//     infinite energy, infinite bombs / rockets, infinite skill points / shards, infinite
//     finisher (a code cave and a jump to it)
//   from branch feature/infamous-second-son-dash-cheats, src/common/infamous_dash_cheats.h,
//   adapted from Liondarkz/issn_pc (GPL-2.0-or-later), written for CUSA00223 01.07:
//     infinite Neon dash, infinite Video dash, longer Smoke dash (1-5x, 1 = off)
//
// Offsets are in the loaded eboot. Before anything is written the bytes there are compared
// with the game's own: on writes the "on" bytes, off writes the original bytes back. Where the
// bytes differ (another game version, or the emulator's own code patches touched the spot) the
// cheat is "not available for this version". The dash cheats keep their byte signatures but
// not their serial check, so on CUSA00004 each applies only where the bytes match.
//
// The planning is pure (unit-tested on synthetic images); Init and Set write game code with
// VirtualProtect around each write, the way the mouse camera hook does.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Core::SecondSonCheats {

/// The order is also the order of the settings keys (Adaptive::CheatKeys) and of the F1 rows.
enum class Id : std::uint8_t {
    Energy,
    Bombs,
    Shards,
    Finisher,
    NeonDash,
    VideoDash,
    SmokeDash,
    Count,
};
inline constexpr std::size_t NumCheats = static_cast<std::size_t>(Id::Count);

const char* Name(Id id);
/// A line shown under the cheat in the F1 window, or nullptr.
const char* Note(Id id);

struct Write {
    std::uint64_t offset;
    std::vector<std::uint8_t> off; ///< the game's own bytes
    std::vector<std::uint8_t> on;
};

/// The writes of a cheat, in the order they are made when it is switched on (a code cave
/// before the jump to it); switching off goes the other way round. Empty for the Smoke dash,
/// which writes the hero's dash ratio from a thread instead.
std::vector<Write> Writes(Id id);

/// Readable(offset, size): the image range is mapped and readable (the eboot has gaps).
using ReadableFn = std::function<bool(std::uint64_t offset, std::uint64_t size)>;

enum class Found : std::uint8_t {
    Off,       ///< the game's own bytes: can be switched on
    On,        ///< already the cheat's bytes
    Different, ///< not available for this version
};

/// What is at a cheat's sites now. For the finisher, a cave already holding the cheat's bytes
/// with the jump still the game's own counts as off. The Smoke dash is checked by its
/// signatures (on or off are the same bytes).
Found Check(const std::uint8_t* image, std::uint64_t size, Id id, const ReadableFn& readable);

/// The dash cheats' extra conditions beyond their written bytes (signatures around them).
bool SignaturesMatch(const std::uint8_t* image, std::uint64_t size, Id id,
                     const ReadableFn& readable);

/// Smoke dash: where the hero pointer is and where its dash ratio sits in the hero object.
inline constexpr std::uint64_t SmokeHeroGlobal = 0x1d0cfe0;
inline constexpr std::uint64_t SmokeHeroRatio = 0x1f7e0;

#ifndef SECOND_SON_PATCHES_STANDALONE
enum class State : std::uint8_t {
    NotLoaded, ///< the game isn't running yet
    Off,
    On,
    NotAvailable, ///< bytes differ: not available for this version
};

/// Called once the eboot is loaded and the emulator's own patches are in: checks every cheat,
/// then switches on the ones kept on (saved in the settings).
void Init(std::uintptr_t eboot_base, std::uint64_t eboot_size);

State GetState(Id id);
/// Switches a cheat on or off at once. Returns the state after it.
State Set(Id id, bool on);

/// Smoke dash length, 1 (off) to 5 times. Setting it above 1 switches the cheat on.
float SmokeScale();
State SetSmokeScale(float scale);

/// "Cheats: ..." for the log and the session report: what was on this launch and what is now.
std::string Summary();
#endif

} // namespace Core::SecondSonCheats
