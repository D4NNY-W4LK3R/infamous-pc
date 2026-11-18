// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <fmt/format.h>

#include "common/arch.h"
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/mouse_aim.h"
#include "input/camera_hook_signature.h"

#if defined(ARCH_X86_64) && defined(_WIN32)
#define MOUSE_AIM_SUPPORTED 1
#include <windows.h>
#include <xbyak/xbyak.h>
#include "core/cpu_patches.h"
#endif

namespace Core::MouseAim {

namespace {

// Shared with the cave (trampoline area, 64-byte aligned).
struct Mailbox {
    u64 delta;  ///< +0: f32 yaw, f32 pitch (radians); the cave swaps it with 0 every camera update
    float pan;  ///< +8: the stored pan after the cave (read-out)
    float tilt; ///< +12: the stored tilt
    u32 frames; ///< +16: camera updates seen by the cave
};

std::atomic<Mailbox*> g_mailbox{nullptr}; ///< published last (release), after everything below
const volatile u8* g_invert_x = nullptr;
const volatile u8* g_invert_y = nullptr;
std::atomic<float> g_sens_deg{5.12f}; // the user's pick, 2026-10-17 (8 x 0.8 x 0.8)

std::filesystem::path MouseAimPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "mouse-aim.ini";
}

/// The sensitivity the - / = keys saved last (0 = none).
float SavedSensitivity() {
    std::ifstream in{MouseAimPath()};
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("sensitivity=", 0) == 0) {
            return static_cast<float>(std::atof(line.c_str() + 12));
        }
    }
    return 0.0f;
}

float g_sign_x = -1.0f; // mouse right -> turn right (user live, 10-17; SHADPS4_MOUSE_AIM_FLIP=x flips)
float g_sign_y = 1.0f; // mouse up -> look up with the in-game invert off (user live, 10-17)
/// Tick (timer thread): the camera hook ran within the last StaleAfter.
std::atomic<bool> g_live{false};
/// The game's "no auto-follow" byte (eboot .bss 0x800e16c40, read only at 0x80014936e: when set,
/// FUN_800149360 turns the camera's auto-follow off through the game's own off path). The right
/// stick turns follow off through the ManualCamera event (pad+0x1c > 0); the mouse never sets it,
/// so without this the camera keeps swinging behind the player (user live test, 2026-10-17;
/// trace scratch/autocam-20261017). Set while the mouse moves, cleared FollowHold after the last
/// movement so the game's own follow comes back.
volatile u8* g_no_follow = nullptr;
std::atomic<s64> g_last_motion_ns{0};
std::mutex g_status_mutex;
std::string g_status = "not checked yet (the game has not started)";

void SetStatus(std::string text) {
    std::scoped_lock lk{g_status_mutex};
    g_status = std::move(text);
}

/// enabled= in user/mouse-aim.ini; nullopt when the file or the line is missing.
std::optional<bool> IniEnabled() {
    std::ifstream in{MouseAimPath()};
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("enabled=", 0) == 0) {
            return line.size() > 8 && line[8] != '0';
        }
    }
    return std::nullopt;
}
constexpr auto StaleAfter = std::chrono::milliseconds(250);

#ifdef MOUSE_AIM_SUPPORTED
// The cave, at the camera's pan / tilt store. Registers (from the disassembly; Opus review
// 2026-10-17): xmm0 = (pan, tilt, 0, 0) to store, xmm1 = the turn speeds stored next (kept),
// r13 = camera, r12 = camera data, [rsp+0x50] = the context whose +0x1c0 pair limits the tilt
// with the data's +0x1c0 pair (FUN_80014a810). rax, r11, xmm2-xmm5 and the flags are dead here
// (the call at 14ebfa is an import stub): the cave needs no stack (no red-zone hazard).
class CameraCave : public Xbyak::CodeGenerator {
public:
    CameraCave(u8* code, size_t size, Mailbox* mailbox, const void* return_to, bool clamp_tilt)
        : Xbyak::CodeGenerator(size, code) {
        Xbyak::Label store;
        mov(rax, reinterpret_cast<u64>(mailbox));
        xor_(r11d, r11d);
        xchg(r11, qword[rax + offsetof(Mailbox, delta)]); // take the mouse motion, leave 0
        test(r11, r11);
        jz(store, T_NEAR); // no motion: exactly the game's own store
        vmovq(xmm2, r11);
        vaddps(xmm3, xmm0, xmm2); // (pan + yaw, tilt + pitch)
        if (!clamp_tilt) {
            vmovaps(xmm0, xmm3);
            jmp(store, T_NEAR);
        }
        // Tilt limits: lower = max(data.x, ctx.x), upper = min(data.y, ctx.y). The mouse may not
        // push past them; a tilt already outside (the game's own doing) is not pulled in.
        mov(r11, qword[rsp + 0x50]);
        vmovsd(xmm4, qword[r12 + 0x1c0]);
        vmovsd(xmm2, qword[r11 + 0x1c0]);
        vmaxps(xmm5, xmm4, xmm2);
        vminps(xmm4, xmm4, xmm2);
        vpermilps(xmm5, xmm5, 0x00); // lower in every lane
        vpermilps(xmm4, xmm4, 0x55); // upper in every lane
        vminps(xmm5, xmm5, xmm0);    // lo = min(lower, current)
        vmaxps(xmm4, xmm4, xmm0);    // hi = max(upper, current)
        vminps(xmm2, xmm4, xmm3);
        vmaxps(xmm2, xmm2, xmm5);        // clamp(candidate, lo, hi)
        vblendps(xmm0, xmm3, xmm2, 0x2); // pan from the candidate, tilt clamped
        L(store);
        vmovq(qword[rax + offsetof(Mailbox, pan)], xmm0);
        inc(dword[rax + offsetof(Mailbox, frames)]);
        vmovups(xword[r13 + 0x330], xmm0); // the original instruction
        jmp(return_to, T_NEAR);
    }
};
#endif

} // namespace

bool Enabled() {
    static const bool setting = [] {
        if (const char* v = std::getenv("SHADPS4_MOUSE_AIM")) {
            return v[0] != '0';
        }
        return IniEnabled().value_or(true);
    }();
    // Only for inFAMOUS Second Son: other games keep the emulator's usual mouse behaviour.
    const auto serial = Common::ElfInfo::Instance().GameSerial();
    return setting && (serial == "CUSA00223" || serial == "CUSA00004");
}

std::string StatusText() {
    std::scoped_lock lk{g_status_mutex};
    return g_status;
}

bool Installed() {
    return g_mailbox.load(std::memory_order_acquire) != nullptr;
}

#ifdef MOUSE_AIM_SUPPORTED
namespace {
/// Committed, readable memory only: the eboot image has unmapped gaps between its segments.
bool Readable(const u8* base, u64 offset, u64 size) {
    const u8* p = base + offset;
    const u8* const end = p + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 || mbi.Protect == 0) {
            return false;
        }
        p = static_cast<const u8*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}
} // namespace
#endif

void InstallCameraHook(VAddr eboot_base, u64 eboot_size) {
#ifdef MOUSE_AIM_SUPPORTED
    if (!Enabled()) {
        SetStatus("off (turned off in mouse-aim.ini or SHADPS4_MOUSE_AIM=0)");
        return;
    }
    if (Installed()) {
        return;
    }
    const auto match = Input::CameraHook::Match(reinterpret_cast<const u8*>(eboot_base),
                                                eboot_size, &Readable);
    if (!match.SafeToHook()) {
        LOG_WARNING(Core,
                    "Mouse aim: camera hook NOT installed ({}: {}). The mouse drives the right "
                    "stick instead (F7).",
                    Input::CameraHook::ToString(match.kind), match.detail);
        SetStatus("off - " + match.detail + "; the mouse drives the right stick instead");
        return;
    }
    const auto& build = *match.profile;
    const u64 StoreOffset = build.store_offset;
    const u64 InvertXOffset = build.invert_x_offset;
    const u64 InvertYOffset = build.invert_y_offset;
    const u64 NoFollowOffset = build.no_follow_offset;
    u8* const site = reinterpret_cast<u8*>(eboot_base + StoreOffset);
    if (const char* v = std::getenv("SHADPS4_MOUSE_AIM_SENS")) {
        const float s = static_cast<float>(std::atof(v));
        if (s > 0.0f) {
            g_sens_deg.store(std::clamp(s, 0.5f, 100.0f));
        }
    } else if (const float saved = SavedSensitivity(); saved > 0.0f) {
        g_sens_deg.store(std::clamp(saved, 0.5f, 100.0f));
    }
    if (const char* v = std::getenv("SHADPS4_MOUSE_AIM_FLIP")) {
        if (std::strchr(v, 'x') != nullptr) {
            g_sign_x = -g_sign_x;
        }
        if (std::strchr(v, 'y') != nullptr) {
            g_sign_y = -g_sign_y;
        }
    }

    constexpr size_t CaveSize = 192;
    u8* const block = AllocateTrampolineSpace(site, 64 + CaveSize);
    if (block == nullptr) {
        LOG_WARNING(Core, "Mouse aim: no trampoline space - off");
        SetStatus("off - no trampoline space for the camera code cave");
        return;
    }
    auto* mailbox = reinterpret_cast<Mailbox*>(block);
    std::memset(mailbox, 0, sizeof(Mailbox));
    u8* const cave_code = block + 64;
    // SHADPS4_MOUSE_AIM_CLAMP=0: no tilt limit from the cave (if the limit fields misbehave).
    const char* clamp_env = std::getenv("SHADPS4_MOUSE_AIM_CLAMP");
    const bool clamp_tilt = clamp_env == nullptr || clamp_env[0] != '0';
    size_t cave_size = 0;
    try {
        CameraCave cave(cave_code, CaveSize, mailbox, site + 9, clamp_tilt);
        cave_size = cave.getSize();
    } catch (const Xbyak::Error& e) {
        LOG_WARNING(Core, "Mouse aim: cave generation failed ({}) - off", static_cast<int>(e));
        SetStatus("off - code cave generation failed");
        return;
    }

    // jmp rel32 to the cave + a 4-byte nop over the 9-byte store (the game has not started).
    const s64 rel = reinterpret_cast<s64>(cave_code) - reinterpret_cast<s64>(site + 5);
    if (rel < INT32_MIN || rel > INT32_MAX) {
        LOG_WARNING(Core, "Mouse aim: cave out of jump range - off");
        SetStatus("off - code cave out of jump range");
        return;
    }
    u8 patch[9] = {0xe9, 0, 0, 0, 0, 0x0f, 0x1f, 0x40, 0x00};
    const s32 rel32 = static_cast<s32>(rel);
    std::memcpy(patch + 1, &rel32, sizeof(rel32));
    DWORD old_protect = 0;
    if (!VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_protect)) {
        LOG_WARNING(Core, "Mouse aim: VirtualProtect failed - off");
        SetStatus("off - could not write the jump into the game code");
        return;
    }
    std::memcpy(site, patch, sizeof(patch));
    VirtualProtect(site, sizeof(patch), old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));

    if (eboot_size > NoFollowOffset) {
        g_no_follow = reinterpret_cast<volatile u8*>(eboot_base + NoFollowOffset);
    }
    g_invert_x = reinterpret_cast<const volatile u8*>(eboot_base + InvertXOffset);
    g_invert_y = reinterpret_cast<const volatile u8*>(eboot_base + InvertYOffset);
    g_mailbox.store(mailbox, std::memory_order_release);
    SetStatus(std::string("installed - ") + build.name);
    LOG_INFO(Core,
             "Mouse aim: camera hook at {:#x} -> cave {} ({} bytes), mailbox {}, {:.1f} deg per "
             "100 px, signs x {} y {}, tilt clamp {}, follow byte {}",
             eboot_base + StoreOffset, fmt::ptr(cave_code), cave_size, fmt::ptr(mailbox),
             g_sens_deg.load(), g_sign_x, g_sign_y, clamp_tilt,
             fmt::ptr(const_cast<u8*>(g_no_follow)));
#else
    (void)eboot_base;
    (void)eboot_size;
    SetStatus("off - only available in the Windows x64 build");
#endif
}

void Tick() {
    Mailbox* const mb = g_mailbox.load(std::memory_order_acquire);
    if (mb == nullptr) {
        return;
    }
    // Only the timer thread keeps these.
    static u32 last_frames = 0;
    static std::chrono::steady_clock::time_point last_change{};
    const u32 frames = std::atomic_ref<u32>{mb->frames}.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (frames != last_frames) {
        if (last_frames == 0) {
            LOG_INFO(Core, "Mouse aim: the camera hook runs (pan {:.3f}, tilt {:.3f})", mb->pan,
                     mb->tilt);
        }
        last_frames = frames;
        last_change = now;
    }
    const bool live = now - last_change <= StaleAfter;
    if (!live) {
        // Menus, cut-scenes, pause, a long load: motion parked in the mailbox must not turn the
        // camera in one jump when gameplay resumes.
        std::atomic_ref<u64>{mb->delta}.store(0, std::memory_order_relaxed);
    }
    // SHADPS4_MOUSE_AIM_FOLLOW_HOLD = seconds the game's auto-follow stays off after the last
    // mouse movement (default 5, the user's pick 2026-10-17; 0 = never touch it).
    static const s64 hold_ns = [] {
        const char* v = std::getenv("SHADPS4_MOUSE_AIM_FOLLOW_HOLD");
        const double s = v ? std::atof(v) : 5.0;
        return static_cast<s64>(std::clamp(s, 0.0, 100000.0) * 1e9);
    }();
    if (g_no_follow != nullptr && hold_ns > 0) {
        const s64 now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               now.time_since_epoch())
                               .count();
        const s64 last = g_last_motion_ns.load(std::memory_order_relaxed);
        const u8 want = last != 0 && now_ns - last < hold_ns ? 1 : 0;
        static u8 written = 0;
        if (want != written) { // only on changes: the game itself never writes it (measured)
            *g_no_follow = want;
            written = want;
        }
    }
    if (live != g_live.load(std::memory_order_relaxed)) {
        g_live.store(live, std::memory_order_relaxed);
        LOG_DEBUG(Core, "Mouse aim: camera {}", live ? "live" : "idle");
    }
}

void OnMouseMotion(float xrel, float yrel) {
    Mailbox* const mb = g_mailbox.load(std::memory_order_acquire);
    if (mb == nullptr || (xrel == 0.0f && yrel == 0.0f) ||
        !g_live.load(std::memory_order_relaxed)) {
        return;
    }
    g_last_motion_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count(),
                           std::memory_order_relaxed);
    const float k = g_sens_deg.load(std::memory_order_relaxed) *
                    (std::numbers::pi_v<float> / 180.0f) / 100.0f;
    float yaw = xrel * k * g_sign_x;
    float pitch = yrel * k * g_sign_y;
    if (*g_invert_x != 0) {
        yaw = -yaw; // the game's own invert options (the stick's sign flip, 14ddd0)
    }
    if (*g_invert_y != 0) {
        pitch = -pitch;
    }
    std::atomic_ref<u64> delta{mb->delta};
    u64 old = delta.load(std::memory_order_relaxed);
    u64 next;
    do {
        float f[2];
        std::memcpy(f, &old, sizeof(f));
        f[0] = std::clamp(f[0] + yaw, -0.75f, 0.75f);
        f[1] = std::clamp(f[1] + pitch, -0.75f, 0.75f);
        std::memcpy(&next, f, sizeof(next));
    } while (!delta.compare_exchange_weak(old, next, std::memory_order_relaxed));
}

float ScaleSensitivity(float factor) {
    float s = g_sens_deg.load(std::memory_order_relaxed);
    s = std::clamp(s * factor, 0.5f, 100.0f);
    g_sens_deg.store(s, std::memory_order_relaxed);
    // Kept for the next start (1.1, user 10-21; the - / = keys were session-only):
    // user/mouse-aim.ini; SHADPS4_MOUSE_AIM_SENS in the flags file still wins.
    const std::optional<bool> enabled = IniEnabled();
    std::ofstream out{MouseAimPath(), std::ios::trunc};
    out << "# Native mouse camera. Written by the - / = keys: mouse-look speed in degrees per 100\n"
           "# pixels (default 5.12). SHADPS4_MOUSE_AIM_SENS in the environment wins over this file.\n"
           "# enabled=0 turns the camera hook off (the mouse then drives the right stick).\n"
        << "enabled=" << (enabled.value_or(true) ? 1 : 0) << "\n"
        << "sensitivity=" << s << "\n";
    LOG_INFO(Core, "Mouse aim: {:.2f} deg per 100 px (saved)", s);
    return s;
}

} // namespace Core::MouseAim
