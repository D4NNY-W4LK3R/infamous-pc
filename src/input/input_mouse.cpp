// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/types.h"
#include "core/mouse_aim.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "input/touch_grip.h"
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

namespace {
std::atomic<SprayHold> g_spray_hold{SprayHold::Normal};
} // namespace

SprayHold CycleSprayHold() {
    const auto next = static_cast<SprayHold>((static_cast<int>(g_spray_hold.load()) + 1) % 3);
    g_spray_hold.store(next);
    return next;
}

void MouseToRotation(SprayHold hold, float d_x, float d_y, float out[3]) {
    switch (hold) {
    case SprayHold::Normal:
        // Pitch (x) for up / down, yaw (y) for left / right.
        out[0] = -d_y / 100;
        out[1] = -d_x / 100;
        break;
    case SprayHold::Sideways:
        // Turned on its side, right grip up: the controller's x axis points up, so turning
        // left / right is a rotation about x and up / down one about y.
        out[0] = -d_x / 100;
        out[1] = d_y / 100;
        break;
    case SprayHold::SidewaysOtherWay:
        out[0] = d_x / 100;
        out[1] = -d_y / 100;
        break;
    }
    out[2] = 0.0f;
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
        float gyro_from_mouse[3];
        MouseToRotation(g_spray_hold.load(std::memory_order_relaxed), d_x, d_y, gyro_from_mouse);
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
    float gyro_from_mouse[3];
    MouseToRotation(g_spray_hold.load(std::memory_order_relaxed), d_x, d_y, gyro_from_mouse);
    if (mouse_gyro_roll_mode) {
        gyro_from_mouse[1] = 0.0f;
        gyro_from_mouse[2] = -d_x / 100;
    }
    controller->SetEmulatedRotationRate(gyro_from_mouse);
}

namespace {
std::mutex g_touch_view_mutex;
TouchModeView g_touch_view;
// What touch mode itself last put on the pad, so it only writes when that changes and leaves
// the keyboard's touchpad keys (R / Y / Tab) alone otherwise.
bool g_touch_finger[2] = {false, false};
bool g_touch_pressed = false;
bool g_was_touch_mode = false;

void PointerOnPad(float* px, float* py) {
    float x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    int w = 0, h = 0;
    SDL_GetWindowSize(g_window->GetSDLWindow(), &w, &h); // same units as the mouse position
    *px = std::clamp(x / static_cast<float>(std::max(w, 1)), 0.0f, 1.0f);
    *py = std::clamp(y / static_cast<float>(std::max(h, 1)), 0.0f, 1.0f);
}

void LiftTouchMode(GameController* controller) {
    for (int i = 0; i < 2; ++i) {
        if (g_touch_finger[i]) {
            controller->SetTouchpadState(i, false, 0.5f, 0.5f);
            g_touch_finger[i] = false;
        }
    }
    if (g_touch_pressed) {
        controller->Button(Libraries::Pad::OrbisPadButtonDataOffset::TouchPad, false);
        g_touch_pressed = false;
    }
    std::scoped_lock lk{g_touch_view_mutex};
    g_touch_view = TouchModeView{};
}
} // namespace

TouchModeView GetTouchModeView() {
    std::scoped_lock lk{g_touch_view_mutex};
    return g_touch_view;
}

bool TouchModePointer(float* x, float* y) {
    if (mouse_mode != MouseMode::Touchpad || g_window == nullptr) {
        return false;
    }
    PointerOnPad(x, y);
    return true;
}

void EmulateTouchpad(GameController* controller, u32 interval) {
    float cx = 0.5f, cy = 0.5f;
    PointerOnPad(&cx, &cy);
    const SDL_MouseButtonFlags buttons = SDL_GetMouseState(nullptr, nullptr);
    const bool left = (buttons & SDL_BUTTON_LMASK) != 0;
    const bool right = (buttons & SDL_BUTTON_RMASK) != 0;
    const bool ctrl = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;

    // Left: one finger exactly under the pointer - put it down where the game shows the mark,
    // move, let go (finger scanners, slides). Right: two thumbs, one on each half of the pad
    // ("Move car", launching a car): the second mirrors the pointer, at least 0.3 apart.
    bool want[2] = {left || right, right};
    float fx[2] = {cx, 1.0f - cx};
    const float fy[2] = {cy, cy};
    if (right && std::abs(fx[0] - fx[1]) < 0.3f) {
        fx[0] = cx <= 0.5f ? 0.35f : 0.65f;
        fx[1] = 1.0f - fx[0];
    }
    for (int i = 0; i < 2; ++i) {
        if (want[i] || g_touch_finger[i]) {
            controller->SetTouchpadState(i, want[i], fx[i], fy[i]);
            g_touch_finger[i] = want[i];
        }
    }
    // Ctrl + a button: the finger also presses the pad down (a real touchpad click).
    const bool pressed = ctrl && (left || right);
    if (pressed != g_touch_pressed) {
        controller->Button(Libraries::Pad::OrbisPadButtonDataOffset::TouchPad, pressed);
        g_touch_pressed = pressed;
    }

    std::scoped_lock lk{g_touch_view_mutex};
    auto& v = g_touch_view;
    const bool was_down = v.down[0];
    v.active = true;
    v.cursor_x = cx;
    v.cursor_y = cy;
    for (int i = 0; i < 2; ++i) {
        v.down[i] = want[i];
        v.x[i] = fx[i];
        v.y[i] = fy[i];
    }
    v.pressed = pressed;
    if (want[0]) {
        if (!was_down) {
            v.trail_count = 0;
        }
        if (v.trail_count == TouchModeView::TrailLength) {
            std::move(v.trail_x + 1, v.trail_x + TouchModeView::TrailLength, v.trail_x);
            std::move(v.trail_y + 1, v.trail_y + TouchModeView::TrailLength, v.trail_y);
            --v.trail_count;
        }
        v.trail_x[v.trail_count] = fx[0];
        v.trail_y[v.trail_count] = fy[0];
        ++v.trail_count;
    }
}

namespace {
std::mutex g_grip_mutex;
TouchGrip g_grip{[] {
    TouchGripConfig c{};
    // SS_TOUCH_GRIP_PX: mouse pixels for the whole width of the pad (default 600).
    if (const char* v = std::getenv("SS_TOUCH_GRIP_PX")) {
        const float px = static_cast<float>(std::atof(v));
        if (px >= 50.0f && px <= 10000.0f) {
            c.px_per_pad_width = px;
        }
    }
    return c;
}()};
bool g_grip_swallowed[8] = {}; ///< left / right presses that began during a grip
// What the grip last put on the pad, so it only writes while it owns the touch.
bool g_grip_finger[2] = {false, false};
bool g_grip_click = false;

double GripNow() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool IsGripButton(u8 b) {
    return b == SDL_BUTTON_MIDDLE || b == SDL_BUTTON_X1;
}

void ApplyTouchGrip(GameController* controller) {
    PadTouch out;
    {
        std::scoped_lock lk{g_grip_mutex};
        out = g_grip.Output(GripNow());
    }
    for (int i = 0; i < 2; ++i) {
        if (out.down[i] || g_grip_finger[i]) {
            controller->SetTouchpadState(i, out.down[i], out.x[i], out.y[i]);
            g_grip_finger[i] = out.down[i];
        }
    }
    if (out.click != g_grip_click) {
        controller->Button(Libraries::Pad::OrbisPadButtonDataOffset::TouchPad, out.click);
        g_grip_click = out.click;
    }
}
} // namespace

bool TouchGripButton(u8 sdl_button, bool down) {
    if (mouse_mode == MouseMode::Touchpad || sdl_button >= std::size(g_grip_swallowed)) {
        return false; // Delete's touch mode has its own use for the buttons
    }
    std::scoped_lock lk{g_grip_mutex};
    if (IsGripButton(sdl_button)) {
        const double now = GripNow();
        if (down) {
            if (!g_grip.Gripping()) {
                g_grip.Begin(now);
            }
        } else if (g_grip.Gripping()) {
            g_grip.End(now);
            const auto& g = g_grip.Last();
            LOG_INFO(Input,
                     "touch grip: {:.0f} ms, finger moved ({:+.2f}, {:+.2f}) of the pad, {}{}{}",
                     g.seconds * 1000.0, g.dx, g.dy, g.slid ? "slid" : "held in place",
                     g.two_fingers ? ", two fingers" : "",
                     g.pressed_at_end ? ", pad pressed at the end" : "");
        }
        return true;
    }
    // The left button stays R2 during a grip: the Mobile Command core is held up on the pad
    // while R2 is pressed again and again.
    if (sdl_button != SDL_BUTTON_RIGHT) {
        return false;
    }
    if (down && g_grip.Gripping()) {
        g_grip_swallowed[sdl_button] = true;
        g_grip.SetSecondFinger(true);
        return true;
    }
    if (!down && g_grip_swallowed[sdl_button]) {
        g_grip_swallowed[sdl_button] = false;
        g_grip.SetSecondFinger(false);
        return true; // the press never reached the bindings, so neither does the release
    }
    return false;
}

bool TouchGripMotion(float xrel, float yrel) {
    std::scoped_lock lk{g_grip_mutex};
    if (!g_grip.Gripping()) {
        return false;
    }
    g_grip.Move(xrel, yrel);
    return true;
}

bool TouchGripActive() {
    std::scoped_lock lk{g_grip_mutex};
    return g_grip.Gripping();
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
    // Touch mode: the pointer is visible and moves over the FPS counter and other overlays, so
    // its clicks always belong to the pad there.
    return mouse_mode == MouseMode::Touchpad || (TouchpadBlockFix() && mouse_mode != MouseMode::Off);
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
    if (g_was_touch_mode && mouse_mode != MouseMode::Touchpad) {
        LiftTouchMode(controller); // leaving touch mode with a finger down lifts it
    }
    g_was_touch_mode = mouse_mode == MouseMode::Touchpad;
    if (TouchGripActive()) {
        // The grip has the mouse: its motion is the finger, not the stick or the motion sensor.
        SDL_GetRelativeMouseState(nullptr, nullptr);
    }
    ApplyTouchGrip(controller);
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
