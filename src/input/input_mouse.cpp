// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <cstdlib>

#include "common/assert.h"
#include "common/types.h"
#include "core/mouse_aim.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "input_mouse.h"

#include <common/singleton.h>
#include <emulator.h>
#include "SDL3/SDL.h"

extern Frontend::WindowSDL* g_window;

namespace Input {

extern std::list<std::pair<InputEvent, bool>> pressed_keys;

int mouse_joystick_binding = 0;
float mouse_deadzone_offset = 0.5, mouse_speed = 1, mouse_speed_offset = 0.1250;
bool mouse_gyro_roll_mode = false;
Uint32 mouse_polling_id = 0;
MouseMode mouse_mode = MouseMode::Off;
bool g_mouse_aim_clicks = false;
/// The mouse turns the emulated motion sensor while the camera hook is idle (EmulateJoystick).
static bool g_motion_from_mouse = false;

// Switches mouse to a set mode or turns mouse emulation off if it was already in that mode.
// Returns whether the mode is turned on.
bool ToggleMouseModeTo(MouseMode m) {
    if (mouse_mode == m) {
        mouse_mode = MouseMode::Off;
    } else {
        mouse_mode = m;
    }
    return mouse_mode == m;
}

MouseMode GetMouseMode() {
    return mouse_mode;
}

bool IsMouseJoystickMode() {
    return mouse_mode == MouseMode::Joystick;
}

bool IsMouseGyroMode() {
    return mouse_mode == MouseMode::Gyro;
}

bool MouseAimActive() {
    return Core::MouseAim::Installed() && mouse_mode == MouseMode::Joystick &&
           mouse_joystick_binding != 1;
}

void SetMouseToJoystick(int joystick) {
    mouse_joystick_binding = joystick;
}

void SetMouseParams(float mdo, float ms, float mso) {
    mouse_deadzone_offset = mdo;
    mouse_speed = ms;
    mouse_speed_offset = mso;
}

void SetMouseGyroRollMode(bool mode) {
    mouse_gyro_roll_mode = mode;
}

void EmulateJoystick(GameController* controller, u32 interval) {
    float d_x = 0, d_y = 0;
    SDL_GetRelativeMouseState(&d_x, &d_y);

    // inFAMOUS Second Son (native mouse camera build): the mouse always does something useful.
    //  - gameplay camera live and the hook installed: the hook turns the camera 1:1 (sdl_window);
    //  - otherwise (spray can, menus, cut-scenes, or a game build the hook does not know): the
    //    mouse drives the right stick AND turns the emulated motion sensor, so the spray can's
    //    pointer follows the mouse and the camera still moves if the hook cannot.
    bool& motion_from_mouse = g_motion_from_mouse;
    const bool second_son = Core::MouseAim::Enabled();
    if (second_son && mouse_joystick_binding != 1) {
        const bool hook_drives_camera = Core::MouseAim::Installed() && Core::MouseAim::CameraLive();
        if (hook_drives_camera) {
            if (motion_from_mouse) {
                constexpr float no_rotation[3] = {0.0f, 0.0f, 0.0f};
                controller->SetEmulatedRotationRate(no_rotation);
                controller->Axis(Axis::RightX, GetAxis(-0x80, 0x7f, 0), false);
                controller->Axis(Axis::RightY, GetAxis(-0x80, 0x7f, 0), false);
                motion_from_mouse = false;
            }
            return;
        }
        float gyro_from_mouse[3] = {-d_y / 100, -d_x / 100, 0.0f};
        controller->SetEmulatedRotationRate(gyro_from_mouse);
        motion_from_mouse = true;
    }

    Axis axis_x, axis_y;
    switch (mouse_joystick_binding) {
    case 1:
        axis_x = Axis::LeftX;
        axis_y = Axis::LeftY;
        break;
    case 2:
        axis_x = Axis::RightX;
        axis_y = Axis::RightY;
        break;
    default:
        if (!second_son) {
            return; // no update needed
        }
        axis_x = Axis::RightX; // Second Son: the camera stick unless the config says otherwise
        axis_y = Axis::RightY;
        break;
    }

    float output_speed =
        SDL_clamp(sqrt(d_x * d_x + d_y * d_y) * mouse_speed + mouse_speed_offset * 128,
                  mouse_deadzone_offset * 128, 128.0);

    float angle = atan2(d_y, d_x);
    float a_x = cos(angle) * output_speed, a_y = sin(angle) * output_speed;

    if (d_x != 0 || d_y != 0) {
        controller->Axis(axis_x, GetAxis(-0x80, 0x7f, a_x), false);
        controller->Axis(axis_y, GetAxis(-0x80, 0x7f, a_y), false);
    } else {
        controller->Axis(axis_x, GetAxis(-0x80, 0x7f, 0), false);
        controller->Axis(axis_y, GetAxis(-0x80, 0x7f, 0), false);
    }
}
void EmulateGyro(GameController* controller, u32 interval) {
    float d_x = 0, d_y = 0;
    SDL_GetRelativeMouseState(&d_x, &d_y);
    float gyro_from_mouse[3] = {-d_y / 100, -d_x / 100, 0.0f};
    if (mouse_gyro_roll_mode) {
        gyro_from_mouse[1] = 0.0f;
        gyro_from_mouse[2] = -d_x / 100;
    }
    controller->SetEmulatedRotationRate(gyro_from_mouse);
}

void EmulateTouchpad(GameController* controller, u32 interval) {
    float x, y;
    SDL_MouseButtonFlags mouse_buttons = SDL_GetMouseState(&x, &y);
    controller->SetTouchpadState(0, (mouse_buttons & SDL_BUTTON_LMASK) != 0,
                                 std::clamp(x / g_window->GetWidth(), 0.0f, 1.0f),
                                 std::clamp(y / g_window->GetHeight(), 0.0f, 1.0f));
    controller->Button(Libraries::Pad::OrbisPadButtonDataOffset::TouchPad,
                       (mouse_buttons & SDL_BUTTON_RMASK) != 0);
}

static bool TouchpadBlockFix() {
    static const bool block_fix = [] {
        const char* v = std::getenv("SHADPS4_TOUCHPAD_BLOCK_FIX");
        return v && v[0] == '1';
    }();
    return block_fix;
}

bool MouseEventsBelongToPad() {
    // A click meant for the pad can land on an overlay window (the FPS counter) and start an
    // ImGui window drag that swallows the keyboard while the button is held.
    return TouchpadBlockFix() && mouse_mode != MouseMode::Off;
}

void ApplyMouseInputBlockers() {
    if (g_mouse_aim_clicks && mouse_mode == MouseMode::Off) {
        // Native mouse camera: the click bindings count only while the mouse is captured.
        for (auto& k : pressed_keys) {
            if (k.first.input.type == InputType::KeyboardMouse &&
                (k.first.input.sdl_id == SDL_BUTTON_LEFT ||
                 k.first.input.sdl_id == SDL_BUTTON_RIGHT)) {
                k.second = true;
            }
        }
    }
    switch (mouse_mode) {
    case MouseMode::Touchpad:
        for (auto& k : pressed_keys) {
            if (k.first.input.sdl_id == SDL_BUTTON_LEFT ||
                k.first.input.sdl_id == SDL_BUTTON_RIGHT) {
                k.second = true;
            }
        }
        break;
    default:
        break;
    }
}

Uint32 MousePolling(void* param, Uint32 id, Uint32 interval) {
    auto* controller = (GameController*)param;
    Core::MouseAim::Tick();
    static bool was_gyro = false;
    if (was_gyro && mouse_mode != MouseMode::Gyro) {
        // Otherwise the last mouse movement keeps turning the controller after gyro mode is off
        constexpr float no_rotation[3] = {0.0f, 0.0f, 0.0f};
        controller->SetEmulatedRotationRate(no_rotation);
    }
    was_gyro = mouse_mode == MouseMode::Gyro;
    if (g_motion_from_mouse && mouse_mode != MouseMode::Joystick) {
        // Mouse released (F7) or another mode: stop turning the controller.
        if (mouse_mode != MouseMode::Gyro) {
            constexpr float no_rotation[3] = {0.0f, 0.0f, 0.0f};
            controller->SetEmulatedRotationRate(no_rotation);
        }
        g_motion_from_mouse = false;
    }
    switch (mouse_mode) {
    case MouseMode::Joystick:
        EmulateJoystick(controller, interval);
        break;
    case MouseMode::Gyro:
        EmulateGyro(controller, interval);
        break;
    case MouseMode::Touchpad:
        EmulateTouchpad(controller, interval);
        break;

    default:
        break;
    }
    return interval;
}

} // namespace Input
