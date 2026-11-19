// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The native mouse camera's code cave and the mailbox it shares with the emulator. Kept apart
// from mouse_aim.cpp (Windows-only installation) so the generated code can be run in a test.

#include <cstddef>

#include "common/types.h"

#if defined(__x86_64__) || defined(_M_X64)
#define MOUSE_AIM_CAVE_AVAILABLE 1
#include <xbyak/xbyak.h>
#endif

namespace Core::MouseAim {

// Shared with the cave (trampoline area, 64-byte aligned).
struct Mailbox {
    u64 delta;  ///< +0: f32 yaw, f32 pitch (radians); the cave swaps it with 0 every camera update
    float pan;  ///< +8: the stored pan after the cave (read-out)
    float tilt; ///< +12: the stored tilt
    u32 frames; ///< +16: camera updates seen by the cave
    u32 hold;   ///< +20: nonzero = keep the tilt the mouse set (the game's own tilt is ignored)
    u64 camera; ///< +24: the camera (r13) of the last store; the tilt is held only for that one
};
static_assert(offsetof(Mailbox, hold) == 20 && offsetof(Mailbox, camera) == 24);

#ifdef MOUSE_AIM_CAVE_AVAILABLE
// The cave, at the camera's pan / tilt store. Registers (from the disassembly; Opus review
// 2026-10-17): xmm0 = (pan, tilt, 0, 0) to store, xmm1 = the turn speeds stored next (kept),
// r13 = camera, r12 = camera data, [rsp+0x50] = the context whose +0x1c0 pair limits the tilt
// with the data's +0x1c0 pair (FUN_80014a810). rax, r11, xmm2-xmm5 and the flags are dead here
// (the call at 14ebfa is an import stub): the cave needs no stack (no red-zone hazard).
class CameraCave : public Xbyak::CodeGenerator {
public:
    CameraCave(u8* code, size_t size, Mailbox* mailbox, const void* return_to, bool clamp_tilt)
        : Xbyak::CodeGenerator(size, code) {
        Xbyak::Label store, no_hold, add;
        mov(rax, reinterpret_cast<u64>(mailbox));
        xor_(r11d, r11d);
        xchg(r11, qword[rax + offsetof(Mailbox, delta)]); // take the mouse motion, leave 0
        vmovaps(xmm3, xmm0); // the candidate starts as the game's (pan, tilt)
        // Tilt hold: same camera as last time and the mouse moved lately: the tilt stored last
        // replaces the game's (its pull back to level on ledges is ignored).
        cmp(dword[rax + offsetof(Mailbox, hold)], 0);
        je(no_hold, T_NEAR);
        cmp(r13, qword[rax + offsetof(Mailbox, camera)]);
        jne(no_hold, T_NEAR);
        vinsertps(xmm3, xmm3, dword[rax + offsetof(Mailbox, tilt)], 0x10); // lane 1 = held tilt
        jmp(add, T_NEAR); // through the tilt limits below, even without motion
        L(no_hold);
        test(r11, r11);
        jz(store, T_NEAR); // no motion: exactly the game's own store
        L(add);
        vmovq(xmm2, r11);
        vaddps(xmm3, xmm3, xmm2); // (pan + yaw, tilt + pitch)
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
        mov(qword[rax + offsetof(Mailbox, camera)], r13);
        inc(dword[rax + offsetof(Mailbox, frames)]);
        vmovups(xword[r13 + 0x330], xmm0); // the original instruction
        jmp(return_to, T_NEAR);
    }
};
#endif // MOUSE_AIM_CAVE_AVAILABLE

} // namespace Core::MouseAim
