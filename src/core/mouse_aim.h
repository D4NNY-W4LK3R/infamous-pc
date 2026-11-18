// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "common/types.h"

// Native mouse camera for inFAMOUS Second Son: true 1:1 mouse look (ported from the
// shadPS4-second-son v1.1.1 native keyboard/mouse fork onto infamous-pc).
// The gameplay camera update turns the right stick into pan / tilt speeds and stores the
// integrated angles at camera+0x330 in one instruction. A code cave at that store adds the
// mouse's yaw / pitch, so mouse motion bypasses the stick curve, sensitivity, acceleration and
// aim assist while a pad's stick keeps the game's feel.
//
// Safety: the hook is installed only when input/camera_hook_signature.cpp recognizes the eboot
// as a build whose code AND data offsets were measured. Any other executable - another game
// version, a relocated build, another game - gets no hook; the mouse then drives the right
// stick (mouse-to-joystick), as in the unmodified emulator.
//
// On by default (it can only act on a recognized build). Turn it off with SHADPS4_MOUSE_AIM=0 or
// "enabled=0" in user/mouse-aim.ini. Mouse look is active while the mouse is captured (F7).
// SHADPS4_MOUSE_AIM_SENS = degrees per 100 px (default 5.12); SHADPS4_MOUSE_AIM_FLIP = x, y or xy.
namespace Core::MouseAim {

bool Enabled();

/// Patches the eboot (base address) once all modules are loaded, before the game's entry.
void InstallCameraHook(VAddr eboot_base, u64 eboot_size);

/// The hook is in place (the game's code matched).
bool Installed();

/// Mouse timer (33 ms): notices when the camera stops / resumes updating; parked motion is
/// cleared while it does not run (menus, cut-scenes).
void Tick();

/// Main thread: relative mouse motion in pixels while mouse aim is on.
void OnMouseMotion(float xrel, float yrel);

/// - / = keys: multiplies the sensitivity; returns the new degrees per 100 px.
float ScaleSensitivity(float factor);

/// One line for the log / overlay: installed, or why not.
std::string StatusText();

} // namespace Core::MouseAim
