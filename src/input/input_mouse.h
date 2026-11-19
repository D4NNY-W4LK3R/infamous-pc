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

/// Mouse touch mode (Delete): the game window is the touchpad.
/// Left button = one finger at the pointer (move while holding to slide it), right button = two
/// thumbs, one on each half, Ctrl + a button also presses the pad down, the wheel swipes from
/// the pointer. The on-screen touchpad (imgui/touchpad_layer) shows it.
struct TouchModeView {
    bool active = false;
    float cursor_x = 0.5f, cursor_y = 0.5f; ///< 0..1 across the window / pad
    bool down[2] = {false, false};
    float x[2] = {0.5f, 0.5f}, y[2] = {0.5f, 0.5f};
    bool pressed = false; ///< the pad is clicked down
    static constexpr int TrailLength = 48;
    int trail_count = 0;                 ///< recent positions of finger 1 while down, oldest first
    float trail_x[TrailLength]{}, trail_y[TrailLength]{};
};
TouchModeView GetTouchModeView();
/// In touch mode: where the pointer is on the pad (0..1). False otherwise.
bool TouchModePointer(float* x, float* y);

/// Touch grip (input/touch_grip.h), in every mouse mode except Delete's touch mode: hold the
/// middle mouse button or the back side button (X1) and the mouse is a finger on the touchpad -
/// tap, hold, swipe any way, slide to a mark; right button adds a second finger, the left button
/// stays R2. These return true when the event belongs to the grip, so the bindings and the
/// camera never see it.
bool TouchGripButton(u8 sdl_button, bool down);
bool TouchGripMotion(float xrel, float yrel);
/// The grip button is held.
bool TouchGripActive();

/// How the emulated controller is held while the mouse turns the motion sensor (spray can).
/// Second Son's stencil art is painted with the controller turned on its side like a spray can,
/// which swaps the motion sensor's axes: with the normal mapping, moving the mouse up and down
/// moved the spray sideways (player report, build 28). P cycles through the three.
enum class SprayHold : u8 { Normal, Sideways, SidewaysOtherWay };
SprayHold CycleSprayHold();
/// The motion sensor's rotation rates (rad/s-ish units of the emulation) for a mouse movement.
void MouseToRotation(SprayHold hold, float d_x, float d_y, float out[3]);

// Polls the mouse for changes
Uint32 MousePolling(void* param, Uint32 id, Uint32 interval);

} // namespace Input
