// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// The dash cheats' offsets and signatures are adapted from Liondarkz/issn_pc (src/dash_hook.h,
// src/hero_hook.h and gpu/shim/issn_hero.cpp), GPL-2.0-or-later, by way of
// src/common/infamous_dash_cheats.h on branch feature/infamous-second-son-dash-cheats. The other
// cheats come from the GoldHEN cheat database (CUSA00004_01.07_2.json, Weysincha, 2B).

#include <algorithm>
#include <cstring>

#include "core/second_son_cheats.h"

#ifndef SECOND_SON_PATCHES_STANDALONE
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

#include <fmt/format.h>

#include "adaptive/emulator_glue.h"
#include "common/logging/log.h"
#ifdef _WIN32
#include <windows.h>
#endif
#endif

namespace Core::SecondSonCheats {

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes Repeat(std::uint8_t b, std::size_t n) {
    return Bytes(n, b);
}

// Infinite Neon dash: the timeout branch after the ability 0x59 test becomes two NOPs.
constexpr std::uint64_t NeonSite = 0x2ef9a2;
constexpr std::uint64_t NeonBranch = 0x2ef9b7;
constexpr std::array<std::uint8_t, 23> NeonSignature = {
    0x48, 0x8d, 0x3d, 0x4f, 0xb3, 0xc0, 0x00, 0xbe, 0x59, 0x00, 0x00, 0x00,
    0x31, 0xd2, 0xe8, 0xbb, 0x34, 0xef, 0xff, 0x84, 0xc0, 0x74, 0x35};
// Infinite Video dash: the "ja timeout" before the button-held check becomes two NOPs; the
// compare reads a 1.5 limit at VideoLimit.
constexpr std::uint64_t VideoSite = 0x55ddf7;
constexpr std::uint64_t VideoBranch = 0x55ddff;
constexpr std::array<std::uint8_t, 16> VideoSignature = {
    0xc5, 0xf8, 0x2e, 0x05, 0x15, 0x44, 0x58, 0x00, 0x77, 0x10, 0x34, 0x01, 0x74, 0x15, 0xeb, 0x0a};
constexpr std::uint64_t VideoLimit = 0xae2214;
// Smoke dash: the code reading the hero pointer and the dash values, checked before the ratio
// in the hero object is written.
constexpr std::uint64_t HeroReader = 0x2659a5;
constexpr std::uint64_t MagicReader = 0x214b35;
constexpr std::array<std::uint8_t, 16> MagicSignature = {
    0xc5, 0xfa, 0x10, 0x97, 0xe8, 0xa1, 0x01, 0x00, 0xc5, 0xfa, 0x10, 0x8f, 0xf0, 0xa1, 0x01, 0x00};
constexpr std::array<std::uint8_t, 11> HeroReaderTail = {0x48, 0x8b, 0x38, 0xc5, 0xfa, 0x10,
                                                         0x87, 0xf0, 0xa1, 0x01, 0x00};

bool Readable(const ReadableFn& readable, std::uint64_t size, std::uint64_t at, std::uint64_t n) {
    return at <= size && n <= size - at && readable(at, n);
}

bool Equal(const std::uint8_t* image, std::uint64_t size, const ReadableFn& readable,
           std::uint64_t at, const std::uint8_t* bytes, std::size_t n) {
    return Readable(readable, size, at, n) && std::memcmp(image + at, bytes, n) == 0;
}

/// The signature, except the bytes the cheat itself writes (from `skip`, `skip_n` long).
template <std::size_t N>
bool SignatureAround(const std::uint8_t* image, std::uint64_t size, const ReadableFn& readable,
                     std::uint64_t at, const std::array<std::uint8_t, N>& signature,
                     std::size_t skip, std::size_t skip_n) {
    if (!Readable(readable, size, at, N)) {
        return false;
    }
    for (std::size_t i = 0; i < N; ++i) {
        if (i >= skip && i < skip + skip_n) {
            continue;
        }
        if (image[at + i] != signature[i]) {
            return false;
        }
    }
    return true;
}

std::int64_t Disp32(const std::uint8_t* p) {
    std::int32_t d;
    std::memcpy(&d, p, sizeof(d));
    return d;
}

} // namespace

const char* Name(Id id) {
    switch (id) {
    case Id::Energy:
        return "Infinite energy";
    case Id::Bombs:
        return "Infinite bombs / rockets";
    case Id::Shards:
        return "Infinite skill points / shards";
    case Id::Finisher:
        return "Infinite finisher";
    case Id::NeonDash:
        return "Infinite Neon dash";
    case Id::VideoDash:
        return "Infinite Video dash";
    case Id::SmokeDash:
        return "Longer Smoke dash";
    case Id::Count:
        break;
    }
    return "?";
}

const char* Note(Id id) {
    switch (id) {
    case Id::Shards:
        return "Changes your save once the game saves (the points stay spent / kept).";
    case Id::SmokeDash:
        return "1 = off; 3 is what the original port used.";
    default:
        return nullptr;
    }
}

// GoldHEN's CUSA00004_01.07_2.json gives energy, bombs and shards as PS4 addresses (eboot loaded
// at 0x400000) and the finisher as offsets in the eboot; CUSA00004_01.07.json gives the same
// three as offsets. All offsets here are in the loaded eboot (build 53: the three had 0x400000
// too many, so build 52 showed them "not available").
std::vector<Write> Writes(Id id) {
    switch (id) {
    case Id::Energy:
        return {{0x20c7f9,
                 {0xC4, 0xC1, 0x7A, 0x11, 0x84, 0x24, 0xE8, 0xA1, 0x01, 0x00},
                 Repeat(0x90, 10)}};
    case Id::Bombs:
        return {{0x1e35a8,
                 {0x89, 0xB7, 0xDC, 0x00, 0x00, 0x00, 0x80, 0xBF, 0xC9, 0x0A, 0x02, 0x00, 0x00},
                 {0xBE, 0x05, 0x00, 0x00, 0x00, 0x89, 0xB7, 0xDC, 0x00, 0x00, 0x00, 0x90, 0x90}}};
    case Id::Shards:
        return {{0x1e22a4,
                 {0x89, 0x9F, 0xE0, 0x00, 0x00, 0x00, 0x80, 0xBF, 0xC9, 0x0A, 0x02, 0x00, 0x00},
                 {0xBB, 0xE7, 0x03, 0x00, 0x00, 0x89, 0x9F, 0xE0, 0x00, 0x00, 0x00, 0x90, 0x90}}};
    case Id::Finisher:
        // The code cave first, then the jump into it.
        return {{0xcc7800,
                 Repeat(0x00, 12),
                 {0xC7, 0x43, 0x28, 0x07, 0x00, 0x00, 0x00, 0xE9, 0x63, 0xEB, 0x59, 0xFF}},
                {0x266368,
                 {0x89, 0x4B, 0x28, 0x48, 0x8B, 0x7B, 0x18},
                 {0xE9, 0x93, 0x14, 0xA6, 0x00, 0x90, 0x90}}};
    case Id::NeonDash:
        return {{NeonBranch, {0x74, 0x35}, {0x90, 0x90}}};
    case Id::VideoDash:
        return {{VideoBranch, {0x77, 0x10}, {0x90, 0x90}}};
    case Id::SmokeDash:
    case Id::Count:
        break;
    }
    return {};
}

bool SignaturesMatch(const std::uint8_t* image, std::uint64_t size, Id id,
                     const ReadableFn& readable) {
    switch (id) {
    case Id::NeonDash:
        // The lea's target is part of the signature's bytes, so an exact match also places it.
        return SignatureAround(image, size, readable, NeonSite, NeonSignature,
                               NeonBranch - NeonSite, 2) &&
               static_cast<std::int64_t>(NeonSite) + 7 + Disp32(image + NeonSite + 3) == 0xefacf8;
    case Id::VideoDash: {
        if (!SignatureAround(image, size, readable, VideoSite, VideoSignature,
                             VideoBranch - VideoSite, 2) ||
            !Readable(readable, size, VideoLimit, 4)) {
            return false;
        }
        float limit;
        std::memcpy(&limit, image + VideoLimit, sizeof(limit));
        return static_cast<std::int64_t>(VideoSite) + 8 + Disp32(image + VideoSite + 4) ==
                   static_cast<std::int64_t>(VideoLimit) &&
               limit == 1.5f;
    }
    case Id::SmokeDash: {
        static constexpr std::uint8_t LeaRax[3] = {0x48, 0x8d, 0x05};
        return Readable(readable, size, SmokeHeroGlobal, 8) &&
               Equal(image, size, readable, MagicReader, MagicSignature.data(),
                     MagicSignature.size()) &&
               Equal(image, size, readable, HeroReader, LeaRax, sizeof(LeaRax)) &&
               Equal(image, size, readable, HeroReader + 7, HeroReaderTail.data(),
                     HeroReaderTail.size()) &&
               static_cast<std::int64_t>(HeroReader) + 7 + Disp32(image + HeroReader + 3) ==
                   static_cast<std::int64_t>(SmokeHeroGlobal);
    }
    default:
        return true;
    }
}

Found Check(const std::uint8_t* image, std::uint64_t size, Id id, const ReadableFn& readable) {
    if (image == nullptr || !SignaturesMatch(image, size, id, readable)) {
        return Found::Different;
    }
    const auto writes = Writes(id);
    if (writes.empty()) {
        return id == Id::SmokeDash ? Found::Off : Found::Different;
    }
    std::vector<bool> is_off;
    std::vector<bool> is_on;
    for (const auto& w : writes) {
        is_off.push_back(Equal(image, size, readable, w.offset, w.off.data(), w.off.size()));
        is_on.push_back(Equal(image, size, readable, w.offset, w.on.data(), w.on.size()));
    }
    const bool all_off = std::all_of(is_off.begin(), is_off.end(), [](bool b) { return b; });
    const bool all_on = std::all_of(is_on.begin(), is_on.end(), [](bool b) { return b; });
    if (all_on) {
        return Found::On;
    }
    if (all_off) {
        return Found::Off;
    }
    // Finisher: the cave written but the jump to it not (switched off, or a switch cut short).
    if (id == Id::Finisher && is_on[0] && is_off[1]) {
        return Found::Off;
    }
    return Found::Different;
}

#ifndef SECOND_SON_PATCHES_STANDALONE

namespace {

struct Slot {
    State state = State::NotLoaded;
    bool ever_on = false;
};

std::mutex g_mutex;
std::array<Slot, NumCheats> g_slots{};
std::uint8_t* g_image = nullptr;
std::uint64_t g_size = 0;
float g_smoke_scale = 1.0f;

#ifdef _WIN32
bool ImageReadable(std::uint64_t offset, std::uint64_t size) {
    // Committed, readable memory only: the eboot image has unmapped gaps between segments.
    const std::uint8_t* p = g_image + offset;
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
}

/// Writes game code: made writable for the write and put back as it was, then the instruction
/// cache flushed (as the mouse camera hook does).
bool WriteCode(std::uint64_t offset, const Bytes& bytes) {
    void* at = g_image + offset;
    DWORD old_protect = 0;
    if (!VirtualProtect(at, bytes.size(), PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    std::memcpy(at, bytes.data(), bytes.size());
    VirtualProtect(at, bytes.size(), old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), at, bytes.size());
    return true;
}

/// Guest memory the Smoke dash thread reads and writes may go away between levels: checked on
/// every use.
bool Access(std::uintptr_t address, std::size_t n, bool write) {
    if (n > UINTPTR_MAX - address) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof mbi) != sizeof mbi ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    if (address < start || n > mbi.RegionSize - (address - start)) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xff;
    if (write) {
        return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_READWRITE ||
               prot == PAGE_EXECUTE_WRITECOPY;
    }
    return prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
           prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_EXECUTE_WRITECOPY;
}

std::atomic<float> g_smoke_factor{1.0f};
std::atomic<bool> g_smoke_restore{false};
std::jthread g_smoke_worker;

/// Every 50 ms while on: the hero's Smoke dash ratio is the game's own times the factor. A
/// fresh ratio from the game (an upgrade, a new hero object) becomes the new base. Switched off
/// from the window, the game's own ratio is written back; at exit nothing is written, as the
/// guest memory may be gone.
void SmokeLoop(std::stop_token stop) {
    std::uintptr_t hero_seen = 0;
    float base = 0.0f;
    float written = 0.0f;
    const auto hero_ratio = [](std::uintptr_t& hero) -> float* {
        const auto global = reinterpret_cast<std::uintptr_t>(g_image + SmokeHeroGlobal);
        if (!Access(global, sizeof(std::uintptr_t), false)) {
            return nullptr;
        }
        std::memcpy(&hero, reinterpret_cast<const void*>(global), sizeof(hero));
        if (hero < (1ull << 32) || hero >= (1ull << 47) ||
            !Access(hero + SmokeHeroRatio, sizeof(float), true)) {
            return nullptr;
        }
        return reinterpret_cast<float*>(hero + SmokeHeroRatio);
    };
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (stop.stop_requested()) {
            break;
        }
        std::uintptr_t hero = 0;
        float* ratio = hero_ratio(hero);
        if (ratio == nullptr) {
            hero_seen = 0;
            continue;
        }
        float now;
        std::memcpy(&now, ratio, sizeof(now));
        if (!std::isfinite(now) || now <= 0.0f || now >= 100.0f) {
            continue;
        }
        if (hero != hero_seen || now != written) {
            hero_seen = hero;
            base = now;
        }
        const float want = base * g_smoke_factor.load(std::memory_order_relaxed);
        if (want != now) {
            std::memcpy(ratio, &want, sizeof(want));
        }
        written = want;
    }
    if (g_smoke_restore.load() && hero_seen != 0) {
        std::uintptr_t hero = 0;
        float* ratio = hero_ratio(hero);
        float now = 0.0f;
        if (ratio != nullptr && hero == hero_seen) {
            std::memcpy(&now, ratio, sizeof(now));
            if (now == written) {
                std::memcpy(ratio, &base, sizeof(base));
            }
        }
    }
}
#endif

const ReadableFn& GameReadable() {
#ifdef _WIN32
    static const ReadableFn fn = [](std::uint64_t offset, std::uint64_t size) {
        return ImageReadable(offset, size);
    };
#else
    static const ReadableFn fn = [](std::uint64_t, std::uint64_t) { return true; };
#endif
    return fn;
}

std::string SummaryLocked() {
    std::string ever;
    std::string now;
    for (std::size_t i = 0; i < NumCheats; ++i) {
        const Id id = static_cast<Id>(i);
        const std::string name = id == Id::SmokeDash
                                     ? fmt::format("{} ({:.1f}x)", Name(id), g_smoke_scale)
                                     : std::string(Name(id));
        if (g_slots[i].ever_on) {
            ever += (ever.empty() ? "" : ", ") + std::string(Name(id));
        }
        if (g_slots[i].state == State::On) {
            now += (now.empty() ? "" : ", ") + name;
        }
    }
    if (ever.empty()) {
        return "Cheats: none on this launch";
    }
    return "Cheats: on at some point this launch: " + ever +
           "; on now: " + (now.empty() ? std::string{"none"} : now) +
           " (frame rates measured with cheats on may not compare with other sessions)";
}

void Publish() {
    Adaptive::Glue::SetCheatsNote(SummaryLocked());
}

/// With the lock held. Switches a byte cheat; returns its state after.
State SetLocked(Id id, bool on) {
    Slot& slot = g_slots[static_cast<std::size_t>(id)];
    if (slot.state == State::NotLoaded || slot.state == State::NotAvailable) {
        return slot.state;
    }
#ifdef _WIN32
    const Found found = Check(g_image, g_size, id, GameReadable());
    if (found == Found::Different) {
        // Something else wrote there since the check at launch.
        slot.state = State::NotAvailable;
        LOG_WARNING(Core, "Cheat: {} not available (the game code there changed)", Name(id));
        return slot.state;
    }
    const auto writes = Writes(id);
    bool ok = true;
    if (on) {
        for (const auto& w : writes) {
            if (std::memcmp(g_image + w.offset, w.on.data(), w.on.size()) != 0) {
                ok = ok && WriteCode(w.offset, w.on);
            }
        }
    } else {
        // The jump before the cave it goes to.
        for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
            if (std::memcmp(g_image + it->offset, it->off.data(), it->off.size()) != 0) {
                ok = ok && WriteCode(it->offset, it->off);
            }
        }
    }
    if (!ok) {
        LOG_WARNING(Core, "Cheat: {} could not be written (VirtualProtect failed)", Name(id));
        slot.state =
            Check(g_image, g_size, id, GameReadable()) == Found::On ? State::On : State::Off;
        return slot.state;
    }
    slot.state = on ? State::On : State::Off;
    slot.ever_on = slot.ever_on || on;
    LOG_INFO(Core, "Cheat: {} {}", Name(id), on ? "on" : "off");
    return slot.state;
#else
    (void)on;
    return slot.state;
#endif
}

State SetSmokeLocked(float scale) {
    Slot& slot = g_slots[static_cast<std::size_t>(Id::SmokeDash)];
    scale = std::clamp(std::isfinite(scale) ? scale : 1.0f, 1.0f, 5.0f);
    if (slot.state == State::NotLoaded || slot.state == State::NotAvailable) {
        return slot.state;
    }
    const bool was_on = slot.state == State::On;
    g_smoke_scale = scale;
#ifdef _WIN32
    g_smoke_factor.store(scale);
    const bool on = scale > 1.0f;
    if (on && !g_smoke_worker.joinable()) {
        g_smoke_restore.store(false);
        g_smoke_worker = std::jthread{SmokeLoop};
    } else if (!on && g_smoke_worker.joinable()) {
        g_smoke_restore.store(true);
        g_smoke_worker.request_stop();
        g_smoke_worker.join();
        g_smoke_worker = {};
    }
    slot.state = on ? State::On : State::Off;
    slot.ever_on = slot.ever_on || on;
    if (on != was_on) {
        LOG_INFO(Core, "Cheat: {} {}", Name(Id::SmokeDash),
                 on ? fmt::format("on ({:.1f}x)", scale) : std::string{"off"});
    }
#else
    (void)was_on;
#endif
    return slot.state;
}

} // namespace

void Init(std::uintptr_t eboot_base, std::uint64_t eboot_size) {
    std::scoped_lock lock{g_mutex};
    g_image = reinterpret_cast<std::uint8_t*>(eboot_base);
    g_size = eboot_size;
    std::string unavailable;
    for (std::size_t i = 0; i < NumCheats; ++i) {
        const Id id = static_cast<Id>(i);
#ifdef _WIN32
        const Found found = Check(g_image, g_size, id, GameReadable());
#else
        const Found found = Found::Different; // game code is written on Windows only
#endif
        g_slots[i].state = found == Found::Different ? State::NotAvailable
                           : found == Found::On      ? State::On
                                                     : State::Off;
        if (found == Found::Different) {
            unavailable += (unavailable.empty() ? "" : ", ") + std::string(Name(id));
            LOG_INFO(Core, "Cheat: {} not available for this version", Name(id));
        }
    }
    LOG_INFO(Core, "Cheats (F1 > Cheats): {} of {} available for this game version{}{}",
             std::count_if(g_slots.begin(), g_slots.end(),
                           [](const Slot& s) { return s.state != State::NotAvailable; }),
             NumCheats, unavailable.empty() ? "" : "; not available: ", unavailable);
    // Kept on from launch to launch (saved in settings): on before the game's entry runs.
    const std::uint32_t kept = Adaptive::Glue::CheatsKeptOn();
    for (std::size_t i = 0; i < NumCheats; ++i) {
        const Id id = static_cast<Id>(i);
        if (id == Id::SmokeDash || !(kept & (1u << i))) {
            continue;
        }
        if (g_slots[i].state == State::NotAvailable) {
            LOG_WARNING(Core, "Cheat: {} kept on, but not available for this version", Name(id));
            continue;
        }
        SetLocked(id, true);
    }
    const float smoke = Adaptive::Glue::SmokeDashKept();
    if (smoke > 1.0f) {
        if (g_slots[static_cast<std::size_t>(Id::SmokeDash)].state == State::NotAvailable) {
            LOG_WARNING(Core, "Cheat: {} kept on, but not available for this version",
                        Name(Id::SmokeDash));
        } else {
            SetSmokeLocked(smoke);
        }
    }
    LOG_INFO(Core, "{}", SummaryLocked());
    Publish();
}

State GetState(Id id) {
    std::scoped_lock lock{g_mutex};
    return g_slots[static_cast<std::size_t>(id)].state;
}

State Set(Id id, bool on) {
    std::scoped_lock lock{g_mutex};
    State state;
    if (id == Id::SmokeDash) {
        state = SetSmokeLocked(on ? (g_smoke_scale > 1.0f ? g_smoke_scale : 3.0f) : 1.0f);
    } else {
        state = SetLocked(id, on);
    }
    Publish();
    return state;
}

float SmokeScale() {
    std::scoped_lock lock{g_mutex};
    return g_smoke_scale;
}

State SetSmokeScale(float scale) {
    std::scoped_lock lock{g_mutex};
    const State state = SetSmokeLocked(scale);
    Publish();
    return state;
}

std::string Summary() {
    std::scoped_lock lock{g_mutex};
    return SummaryLocked();
}

#endif

} // namespace Core::SecondSonCheats
