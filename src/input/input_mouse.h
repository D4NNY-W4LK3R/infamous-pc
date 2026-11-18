// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "SDL3/SDL.h"
#include "common/types.h"

namespace Input {

enum MouseMode {
    Off = 0,
    Joystick,
    Gyro,
    Touchpad,
};

bool ToggleMouseModeTo(MouseMode m);
MouseMode GetMouseMode();
void SetMouseToJoystick(int joystick);
void SetMouseParams(float mouse_deadzone_offset, float mouse_speed, float mouse_speed_offset);
void SetMouseGyroRollMode(bool mode);

void EmulateJoystick(GameController* controller, u32 interval);
void EmulateGyro(GameController* controller, u32 interval);

void ApplyMouseInputBlockers();
/// SHADPS4_TOUCHPAD_BLOCK_FIX=1: while the mouse drives the pad (touchpad / gyro / stick), its
/// presses and motion belong to the game, not to the ImGui overlay windows.
bool MouseEventsBelongToPad();
/// Native mouse camera: R2 / L2 were added on the left / right mouse button (input_handler.cpp);
/// they count only while the mouse is captured, so the click that focuses the window never shoots.
extern bool g_mouse_aim_clicks;
/// F7 (mouse to joystick) is on.
bool IsMouseJoystickMode();
bool IsMouseGyroMode();
/// Native mouse camera: the hook is installed, F7 is on and the mouse is not bound to the
/// left stick - mouse motion turns the camera directly (sdl_window.cpp).
bool MouseAimActive();

// Polls the mouse for changes
Uint32 MousePolling(void* param, Uint32 id, Uint32 interval);

} // namespace Input
