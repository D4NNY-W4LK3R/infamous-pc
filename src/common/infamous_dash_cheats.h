// SPDX-FileCopyrightText: 2026 infamous-pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// Game-specific opt-in movement cheats for inFAMOUS Second Son CUSA00223 01.07.
// Gameplay offsets/signatures adapted from Liondarkz/issn_pc (src/dash_hook.h,
// src/hero_hook.h and gpu/shim/issn_hero.cpp), GPL-2.0-or-later.
// This operates on already-loaded guest memory; no game executable is distributed.
#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace InfamousDashCheats {

struct Settings {
    bool neon = false;
    bool video = false;
    float smoke_scale = 1.0f; // 1.0 disables; 3.0 is the source port's behavior
};

enum class Status : unsigned char { Disabled, Applied, Unsupported, SignatureMismatch };
struct Result {
    Status neon = Status::Disabled;
    Status video = Status::Disabled;
    Status smoke = Status::Disabled;
};

inline bool Enabled(const char* name) {
    const char* value = std::getenv(name);
    return value && (std::string_view(value) == "1" || std::string_view(value) == "true" ||
                     std::string_view(value) == "TRUE");
}

inline Settings FromEnvironment() {
    Settings out;
    out.neon = Enabled("SHAD_SECOND_SON_INFINITE_NEON_DASH");
    out.video = Enabled("SHAD_SECOND_SON_INFINITE_VIDEO_DASH");
    if (const char* raw = std::getenv("SHAD_SECOND_SON_SMOKE_DASH_SCALE")) {
        char* end = nullptr;
        const float scale = std::strtof(raw, &end);
        if (end && end != raw && *end == '\0' && std::isfinite(scale) && scale >= 1.0f &&
            scale <= 5.0f) {
            out.smoke_scale = scale;
        }
    }
    return out;
}

namespace Detail {
// Offsets refer to the loaded 1.07 eboot image, not ELF file offsets.
inline constexpr std::size_t NeonSite = 0x2ef9a2;
inline constexpr std::size_t NeonBranch = 0x2ef9b7; // je end, after ability 0x59 test
inline constexpr std::array<std::uint8_t, 23> NeonSignature = {
    0x48,0x8d,0x3d,0x4f,0xb3,0xc0,0x00,0xbe,0x59,0x00,0x00,0x00,
    0x31,0xd2,0xe8,0xbb,0x34,0xef,0xff,0x84,0xc0,0x74,0x35};
inline constexpr std::size_t VideoSite = 0x55ddf7;
inline constexpr std::size_t VideoBranch = 0x55ddff; // ja timeout, before button held check
inline constexpr std::array<std::uint8_t, 16> VideoSignature = {
    0xc5,0xf8,0x2e,0x05,0x15,0x44,0x58,0x00,0x77,0x10,
    0x34,0x01,0x74,0x15,0xeb,0x0a};
inline constexpr std::size_t VideoLimit = 0xae2214;
inline constexpr std::size_t HeroReader = 0x2659a5;
inline constexpr std::size_t HeroGlobal = 0x1d0cfe0;
inline constexpr std::size_t HeroRatio = 0x1f7e0;
inline constexpr std::size_t MagicReader = 0x214b35;
inline constexpr std::array<std::uint8_t, 16> MagicSignature = {
    0xc5,0xfa,0x10,0x97,0xe8,0xa1,0x01,0x00,
    0xc5,0xfa,0x10,0x8f,0xf0,0xa1,0x01,0x00};
inline constexpr std::array<std::uint8_t, 11> HeroReaderTail = {
    0x48,0x8b,0x38,0xc5,0xfa,0x10,0x87,0xf0,0xa1,0x01,0x00};

inline bool InImage(std::size_t at, std::size_t n, std::size_t size) {
    return at <= size && n <= size - at;
}

template <std::size_t N>
inline bool Matches(const std::uint8_t* image, std::size_t size, std::size_t at,
                    const std::array<std::uint8_t, N>& pattern) {
    return InImage(at, pattern.size(), size) &&
           std::memcmp(image + at, pattern.data(), pattern.size()) == 0;
}

inline bool NeonMatches(const std::uint8_t* image, std::size_t size) {
    if (!Matches(image,size,NeonSite,NeonSignature)) return false;
    std::int32_t disp;
    std::memcpy(&disp, image + NeonSite + 3, sizeof disp);
    return std::int64_t(NeonSite) + 7 + disp == 0xefacf8;
}
inline bool VideoMatches(const std::uint8_t* image, std::size_t size) {
    if (!Matches(image,size,VideoSite,VideoSignature) || !InImage(VideoLimit, 4, size))
        return false;
    std::int32_t disp;
    float limit;
    std::memcpy(&disp, image + VideoSite + 4, 4);
    std::memcpy(&limit, image + VideoLimit, 4);
    return std::int64_t(VideoSite) + 8 + disp == std::int64_t(VideoLimit) && limit == 1.5f;
}
inline bool SmokeMatches(const std::uint8_t* image, std::size_t size) {
    if (!InImage(HeroGlobal, sizeof(std::uintptr_t), size) ||
        !Matches(image, size, MagicReader, MagicSignature) ||
        !InImage(HeroReader, 18, size)) return false;
    if (std::memcmp(image + HeroReader, "\x48\x8d\x05", 3) ||
        std::memcmp(image + HeroReader + 7, HeroReaderTail.data(), HeroReaderTail.size())) return false;
    std::int32_t disp;
    std::memcpy(&disp, image + HeroReader + 3, 4);
    return std::int64_t(HeroReader) + 7 + disp == std::int64_t(HeroGlobal);
}

#ifdef _WIN32
// The guest hero object is allocated in emulated process memory. Verify host mapping
// on every use; never touch the previous hero after a level/menu transition.
inline bool Access(std::uintptr_t address, std::size_t n, bool write) {
    if (n > UINTPTR_MAX - address) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof mbi) != sizeof mbi ||
        mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    if (address < start || n > mbi.RegionSize - (address - start)) return false;
    const DWORD prot = mbi.Protect & 0xff;
    if (write)
        return prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
               prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    return prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY ||
           prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_EXECUTE_WRITECOPY;
}
inline void SmokeLoop(std::stop_token stop, std::uint8_t* image, float factor) {
    std::uintptr_t previous = 0;
    float written = 1.0f;
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (stop.stop_requested() ||
            !Access(reinterpret_cast<std::uintptr_t>(image + HeroGlobal), sizeof(std::uintptr_t), false))
            break;
        std::uintptr_t hero = 0;
        std::memcpy(&hero, image + HeroGlobal, sizeof hero);
        if (hero < (1ull << 32) || hero >= (1ull << 47) ||
            !Access(hero + HeroRatio, sizeof(float), true)) {
            previous = 0;
            continue;
        }
        float now;
        std::memcpy(&now, reinterpret_cast<const void*>(hero + HeroRatio), sizeof now);
        if (!std::isfinite(now) || now <= 0 || now >= 100.0f) continue;
        // When the game supplies a fresh ratio (e.g. after an upgrade), scale that new base.
        if (previous != hero || now != written) {
            previous = hero;
            written = now * factor;
            std::memcpy(reinterpret_cast<void*>(hero + HeroRatio), &written, sizeof written);
        }
    }
    // No shutdown write: game guest memory may already be unmapped by the emulator.
}
inline std::jthread smoke_worker;
#endif
} // namespace Detail

inline Result Apply(std::string_view serial, std::string_view version,
                    std::uint8_t* image, std::size_t size, const Settings& settings) {
    Result r;
    if (serial != "CUSA00223" || version != "01.07" || image == nullptr) {
        if (settings.neon) r.neon = Status::Unsupported;
        if (settings.video) r.video = Status::Unsupported;
        if (settings.smoke_scale > 1.0f) r.smoke = Status::Unsupported;
        return r;
    }
    if (settings.neon) {
        r.neon = Status::SignatureMismatch;
        if (Detail::NeonMatches(image, size)) {
            image[Detail::NeonBranch] = 0x90; // remove only timeout condition
            image[Detail::NeonBranch + 1] = 0x90;
            r.neon = Status::Applied;
        }
    }
    if (settings.video) {
        r.video = Status::SignatureMismatch;
        if (Detail::VideoMatches(image, size)) {
            image[Detail::VideoBranch] = 0x90;
            image[Detail::VideoBranch + 1] = 0x90;
            r.video = Status::Applied;
        }
    }
    if (settings.smoke_scale > 1.0f) {
        r.smoke = Status::SignatureMismatch;
        if (Detail::SmokeMatches(image, size)) {
#ifdef _WIN32
            if (Detail::smoke_worker.joinable()) Detail::smoke_worker.request_stop();
            if (Detail::smoke_worker.joinable()) Detail::smoke_worker.join();
            Detail::smoke_worker = std::jthread(Detail::SmokeLoop, image, settings.smoke_scale);
            r.smoke = Status::Applied;
#else
            r.smoke = Status::Unsupported; // runtime trainer currently Windows-only
#endif
        }
    }
    return r;
}

} // namespace InfamousDashCheats
