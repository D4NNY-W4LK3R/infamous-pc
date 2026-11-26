// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <cmrc/cmrc.hpp>
#include <stb_image.h>

#include "adaptive/emulator_glue.h"
#include "common/startup_progress.h"
#include "common/assert.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/logging/formatter.h"
#include "common/scope_exit.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/time.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/system/userservice.h"
#include "core/mouse_aim.h"
#include "core/user_settings.h"
#include "imgui/big_picture/settings_dialog_layer.h"
#include "imgui/friends_layer.h"
#include "imgui/controls_hint_layer.h"
#include "imgui/notice_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "input/input_mouse.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"

#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <core/emulator_settings.h>
#include "core/libraries/keyboard/keyboard.h"
#include "core/libraries/mouse/sdl_mouse.h"

CMRC_DECLARE(res);

namespace Frontend {

using namespace Libraries::Pad;

static OrbisPadButtonDataOffset SDLGamepadToOrbisButton(u8 button) {
    using OPBDO = OrbisPadButtonDataOffset;

    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return OPBDO::Down;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return OPBDO::Up;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return OPBDO::Left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return OPBDO::Right;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return OPBDO::Cross;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return OPBDO::Triangle;
    case SDL_GAMEPAD_BUTTON_WEST:
        return OPBDO::Square;
    case SDL_GAMEPAD_BUTTON_EAST:
        return OPBDO::Circle;
    case SDL_GAMEPAD_BUTTON_START:
        return OPBDO::Options;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        return OPBDO::TouchPad;
    case SDL_GAMEPAD_BUTTON_BACK:
        return OPBDO::TouchPad;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return OPBDO::L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return OPBDO::R1;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return OPBDO::L3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return OPBDO::R3;
    default:
        return OPBDO::None;
    }
}

static Uint32 SDLCALL PollController(void* userdata, SDL_TimerID timer_id, Uint32 interval) {
    auto* controller = reinterpret_cast<Input::GameController*>(userdata);
    controller->PollState();
    return interval;
}

static Uint32 SDLCALL PollControllerLightColour(void* userdata, SDL_TimerID timer_id,
                                                Uint32 interval) {
    auto* controller = reinterpret_cast<Input::GameController*>(userdata);
    controller->PollLightColour();
    return interval;
}

namespace {
SDL_Window* g_progress_window = nullptr; ///< for the start-up progress hook
std::string g_progress_title;
} // namespace

WindowSDL::WindowSDL(s32 width_, s32 height_, Input::GameControllers* controllers_,
                     std::string_view window_title)
    : width{width_}, height{height_}, controllers{*controllers_} {
    if (!SDL_SetHint(SDL_HINT_APP_NAME, "shadPS4")) {
        UNREACHABLE_MSG("Failed to set SDL window hint: {}", SDL_GetError());
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        UNREACHABLE_MSG("Failed to initialize SDL video subsystem: {}", SDL_GetError());
    }
    // On macOS, the future Intel compatibility environment does not include camera frameworks.
    // Just skip initializing it entirely, no point in splitting old vs new OS versions here.
#ifndef __APPLE__
    if (!SDL_Init(SDL_INIT_CAMERA)) {
        LOG_ERROR(Input, "Failed to initialize SDL camera subsystem: {}", SDL_GetError());
    }
#endif
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                          std::string(window_title).c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(props, "flags", SDL_WINDOW_VULKAN);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    // Creating the window directly in fullscreen avoids a visible windowed -> fullscreen
    // transition on startup. SDL sizes the window to the display and keeps the requested
    // width/height as the windowed size to restore when leaving fullscreen.
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN,
                           EmulatorSettings.IsFullScreen());
    window = SDL_CreateWindowWithProperties(props);
    // Shader preload progress in the title, and Windows messages handled meanwhile (the preload
    // runs on this thread before the event loop starts).
    g_progress_window = window;
    g_progress_title = std::string{window_title};
    Common::SetStartupProgressHook([](const std::string& text) {
        if (g_progress_window == nullptr) {
            return;
        }
        const std::string title =
            text.empty() ? g_progress_title : g_progress_title + " - " + text;
        SDL_SetWindowTitle(g_progress_window, title.c_str());
        SDL_PumpEvents();
    });
    SDL_DestroyProperties(props);
    if (window == nullptr) {
        UNREACHABLE_MSG("Failed to create window handle: {}", SDL_GetError());
    }

    SDL_SetWindowMinimumSize(window, 640, 360);

    bool error = false;
    const SDL_DisplayID displayIndex = SDL_GetDisplayForWindow(window);
    if (displayIndex == 0) {
        LOG_ERROR(Frontend, "Error getting display index: {}", SDL_GetError());
        error = true;
    }
    const SDL_DisplayMode* displayMode;
    if ((displayMode = SDL_GetCurrentDisplayMode(displayIndex)) == 0) {
        LOG_ERROR(Frontend, "Error getting display mode: {}", SDL_GetError());
        error = true;
    }
    if (!error) {
        // Frame pacing depends on how the display refresh rate lines up with the game's 60 Hz.
        LOG_INFO(Frontend, "Display mode: {}x{} @ {:.2f} Hz, fullscreen: {} ({})", displayMode->w,
                 displayMode->h, displayMode->refresh_rate, EmulatorSettings.IsFullScreen(),
                 EmulatorSettings.GetFullScreenMode());
        SDL_SetWindowFullscreenMode(
            window, EmulatorSettings.GetFullScreenMode() == "Fullscreen" ? displayMode : NULL);
    }
    SDL_SetWindowFullscreen(window, EmulatorSettings.IsFullScreen());
    SDL_SyncWindow(window);
    // The window geometry is only final once the fullscreen transition has settled; refresh
    // the cached size so the first swapchain and the splashscreen use the real drawable size.
    SDL_GetWindowSizeInPixels(window, &width, &height);

    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    SDL_InitSubSystem(SDL_INIT_GAMEPAD);

#if defined(SDL_PLATFORM_WIN32)
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (window_info.render_surface) {
        HWND hwnd = static_cast<HWND>(window_info.render_surface);
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
    }
#elif defined(SDL_PLATFORM_LINUX) || defined(__FreeBSD__)
    // SDL doesn't have a platform define for FreeBSD AAAAAAAAAA
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        window_info.render_surface = (void*)SDL_GetNumberProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    } else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        window_info.render_surface = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    }
#elif defined(SDL_PLATFORM_MACOS)
    window_info.type = WindowSystemType::Metal;
    window_info.render_surface = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
#endif
    // input handler init-s
    Input::ControllerOutput::LinkJoystickAxes();
    Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));

    if (EmulatorSettings.IsBackgroundControllerInput()) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }
}

WindowSDL::~WindowSDL() = default;

void WindowSDL::SetIcon(std::span<const u8> png_data) {
    if (png_data.empty()) {
        LOG_WARNING(Core, "No window icon data available, using default icon.");
        SetDefaultWindowIcon(window);
        return;
    }
    SetWindowIcon(window, std::vector<u8>(png_data.begin(), png_data.end()));
}

namespace {

/// The F3 settings window and the F1 optimizer window need a pointer: while one is open the
/// mouse is released (no mouse look, no stick, visible cursor) and afterwards it goes back to the
/// mode it was in.
void HandMouseToDialogs(SDL_Window* window) {
    static bool dialog_had_mouse = false;
    static Input::MouseMode before = Input::MouseMode::Off;
    const bool dialog =
        ImGuiEmuSettings::IsInGameSettingsDialogOpen() || Adaptive::Glue::OverlayVisible();
    if (dialog == dialog_had_mouse) {
        return;
    }
    dialog_had_mouse = dialog;
    if (dialog) {
        before = Input::GetMouseMode();
        if (before != Input::MouseMode::Off) {
            Input::ToggleMouseModeTo(before); // the same mode again turns it off
        }
        SDL_SetWindowRelativeMouseMode(window, false);
        SDL_ShowCursor();
        return;
    }
    if (before != Input::MouseMode::Off && Input::GetMouseMode() == Input::MouseMode::Off) {
        Input::ToggleMouseModeTo(before);
    }
    const auto now = Input::GetMouseMode();
    SDL_SetWindowRelativeMouseMode(window, now == Input::MouseMode::Joystick ||
                                               now == Input::MouseMode::Gyro);
}

} // namespace

void WindowSDL::WaitEvent() {
    // Called on main thread
    SDL_Event event;

    if (!SDL_WaitEvent(&event)) {
        return;
    }
    HandMouseToDialogs(window);

    // F1 closes its own window even when that window has the keyboard (ImGui would otherwise
    // swallow the key before the hotkey bindings see it).
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
        Adaptive::Glue::OverlayVisible() &&
        Input::IsSingleKeyHotkey(HOTKEY_ADAPTIVE_OVERLAY, event.key.key)) {
        Adaptive::Glue::ToggleOverlay();
        HandMouseToDialogs(window);
        return;
    }

    if (Libraries::Mouse::PushSDLEvent(event) || Libraries::Keyboard::PushSDLEvent(event)) {
        return;
    }

    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        Adaptive::Glue::NoteInput(); // calibration only measures idle seconds
        break;
    default:
        break;
    }

    // SHADPS4_TOUCHPAD_BLOCK_FIX=1: while the mouse drives the pad, overlay windows never see its
    // presses / motion, so a touch can't start a window drag that swallows the keyboard.
    const bool mouse_to_pad =
        Input::MouseEventsBelongToPad() &&
        (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_MOTION ||
         event.type == SDL_EVENT_MOUSE_WHEEL);
    if (!mouse_to_pad && ImGui::Core::ProcessEvent(&event)) {
        return;
    }

    const auto emu_key = [&event](Input::EmuKey k) {
        const u32 key = Input::GetEmuKey(k);
        return key != 0 && event.key.key == key;
    };

    // Touchpad / motion modes hand the mouse back to the camera when they are toggled off, if it
    // had it before (finger scanners, the D.U.P. core lifts, the spray can).
    const auto ToggleSpecialMouseMode = [this](Input::MouseMode m) {
        static Input::MouseMode before = Input::MouseMode::Off;
        const Input::MouseMode prev = Input::GetMouseMode();
        if (Input::ToggleMouseModeTo(m)) {
            if (prev == Input::MouseMode::Joystick) {
                before = prev;
            }
        } else if (before == Input::MouseMode::Joystick) {
            Input::ToggleMouseModeTo(Input::MouseMode::Joystick);
            before = Input::MouseMode::Off;
        }
        const Input::MouseMode now = Input::GetMouseMode();
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(), now == Input::MouseMode::Joystick ||
                                                                 now == Input::MouseMode::Gyro);
    };

    switch (event.type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        OnResize();
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = event.type == SDL_EVENT_WINDOW_EXPOSED;
        OnResize();
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (Input::TouchGripMotion(event.motion.xrel, event.motion.yrel)) {
            break; // the grip has the mouse: it moves the finger, not the camera
        }
        if (Input::MouseAimActive()) {
            // Native mouse camera: every pixel goes straight to the camera's pan / tilt.
            Core::MouseAim::OnMouseMotion(event.motion.xrel, event.motion.yrel);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        // Touch grip: the middle / back button holds a finger on the touchpad; the left and
        // right buttons pressed during it press the pad / add a second finger.
        const bool dialog = ImGuiEmuSettings::IsInGameSettingsDialogOpen() ||
                            Adaptive::Glue::OverlayVisible();
        if ((!dialog || Input::TouchGripActive()) &&
            Input::TouchGripButton(event.button.button,
                                   event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)) {
            // While gripping the pointer is captured, so the finger never stops at the edge of
            // the screen; afterwards the mouse goes back to how it was.
            static bool restore_relative = false;
            static bool grip_was = false;
            const bool grip_now = Input::TouchGripActive();
            if (grip_now && !grip_was) {
                restore_relative = SDL_GetWindowRelativeMouseMode(window);
                SDL_SetWindowRelativeMouseMode(window, true);
            } else if (!grip_now && grip_was) {
                SDL_SetWindowRelativeMouseMode(window, restore_relative);
            }
            grip_was = grip_now;
            break;
        }
        OnKeyboardMouseInput(&event);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_WHEEL_OFF:
        OnKeyboardMouseInput(&event);
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (Input::MouseAimActive() && event.type == SDL_EVENT_KEY_DOWN &&
            (emu_key(Input::EmuKey::MouseAimSlower) || emu_key(Input::EmuKey::MouseAimFaster))) {
            // - / = : mouse-look speed down / up, remembered in user/mouse-aim.ini.
            const float sens = Core::MouseAim::ScaleSensitivity(
                emu_key(Input::EmuKey::MouseAimSlower) ? 0.8f : 1.25f);
            Overlay::Notice::Show(fmt::format("Mouse look: {:.1f} degrees per 100 px", sens));
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            emu_key(Input::EmuKey::SprayCanSideways)) {
            // P: how the spray can (the motion sensor the mouse turns) is held. Stencil art is
            // painted with the controller on its side.
            const auto hold = Input::CycleSprayHold();
            LOG_INFO(Input, "Spray can held {}", static_cast<int>(hold));
            Overlay::Notice::Show(
                hold == Input::SprayHold::Normal     ? "Spray can: held normally (graffiti)"
                : hold == Input::SprayHold::Sideways ? "Spray can: sideways (stencil art) - P again if up/down is reversed"
                                                     : "Spray can: sideways, the other way - P again for normal");
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            Input::MouseTurnsMotionSensor() &&
            (emu_key(Input::EmuKey::SprayFlipUpDown) ||
             emu_key(Input::EmuKey::SprayFlipLeftRight))) {
            // N / M (build 52): flip the mouse's up / down or left / right while painting, for
            // stencils that paint the wrong way. Only while the mouse turns the spray can, so
            // the keys do nothing in gameplay; they reset once back in gameplay.
            const bool up_down = emu_key(Input::EmuKey::SprayFlipUpDown);
            const bool on = up_down ? Input::ToggleSprayFlipUpDown()
                                    : Input::ToggleSprayFlipLeftRight();
            const char* what = up_down ? "up / down" : "left / right";
            LOG_INFO(Input, "Spray flip: {} {}", what, on ? "reversed" : "normal");
            Overlay::Notice::Show(fmt::format("Spray can: mouse {} {} (until the can is put away)",
                                              what, on ? "reversed" : "normal again"));
        }
        OnKeyboardMouseInput(&event);
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        controllers.TryOpenSDLControllers();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        OnGamepadEvent(&event);
        break;
    case SDL_EVENT_QUIT:
        is_open = false;
        break;
    case SDL_EVENT_QUIT_DIALOG:
        Overlay::ToggleQuitWindow();
        break;
    case SDL_EVENT_TOGGLE_FULLSCREEN: {
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) {
            SDL_SetWindowFullscreen(window, 0);
        } else {
            SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN);
        }
        break;
    }
    case SDL_EVENT_TOGGLE_PAUSE:
        if (DebugState.IsGuestThreadsPaused()) {
            LOG_INFO(Frontend, "Game Resumed");
            DebugState.ResumeGuestThreads();
        } else {
            LOG_INFO(Frontend, "Game Paused");
            DebugState.PauseGuestThreads();
        }
        break;
    case SDL_EVENT_CHANGE_CONTROLLER:
        UNREACHABLE_MSG("todo");
        break;
    case SDL_EVENT_TOGGLE_SIMPLE_FPS:
        Overlay::ToggleSimpleFps();
        break;
    case SDL_EVENT_TOGGLE_FRIENDS:
        ImGui::Friends::Toggle();
        break;
    case SDL_EVENT_RELOAD_INPUTS:
        Input::ParseInputConfig(std::string(Common::ElfInfo::Instance().GameSerial()));
        break;
    case SDL_EVENT_MOUSE_TO_JOYSTICK:
        SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(),
                                       Input::ToggleMouseModeTo(Input::MouseMode::Joystick));
        if (Core::MouseAim::Installed()) {
            Overlay::Notice::Show(Input::MouseAimActive() ? "Mouse look ON (- / = speed)"
                                                          : "Mouse look off");
        } else if (Core::MouseAim::Enabled()) {
            Overlay::Notice::Show(Input::IsMouseJoystickMode()
                                      ? "Mouse drives the right stick (camera hook not available "
                                        "for this game build)"
                                      : "Mouse released");
        }
        break;
    case SDL_EVENT_MOUSE_TO_GYRO:
        ToggleSpecialMouseMode(Input::MouseMode::Gyro);
        break;
    case SDL_EVENT_MOUSE_TO_TOUCHPAD:
        ToggleSpecialMouseMode(Input::MouseMode::Touchpad);
        if (Input::GetMouseMode() == Input::MouseMode::Touchpad) {
            SDL_SetWindowRelativeMouseMode(this->GetSDLWindow(), false);
            Overlay::Notice::Show("Mouse = touchpad (Delete again to go back)");
        } else if (Input::MouseAimActive()) {
            Overlay::Notice::Show("Mouse look ON");
        }
        break;
    case SDL_EVENT_TOGGLE_ADAPTIVE_OVERLAY:
        Adaptive::Glue::ToggleOverlay();
        HandMouseToDialogs(window);
        break;
    case SDL_EVENT_TOGGLE_CONTROLS_HINT:
        Overlay::ControlsHint::Toggle();
        break;
    case SDL_EVENT_ADD_VIRTUAL_USER:
        for (int i = 0; i < 4; i++) {
            if (controllers[i]->user_id == -1) {
                auto u = UserManagement.GetUserByPlayerIndex(i + 1);
                if (!u) {
                    break;
                }
                controllers[i]->user_id = u->user_id;
                controllers[i]->ConnectController(controllers[i]->m_sdl_gamepad);
                UserManagement.LoginUser(u, i + 1);
                break;
            }
        }
        break;
    case SDL_EVENT_REMOVE_VIRTUAL_USER:
        LOG_INFO(Input, "Remove user");
        for (int i = 3; i >= 0; i--) {
            if (controllers[i]->user_id != -1) {
                UserManagement.LogoutUser(UserManagement.GetUserByID(controllers[i]->user_id));
                controllers[i]->DisconnectController();
                controllers[i]->user_id = -1;
                break;
            }
        }
        break;
    case SDL_EVENT_RDOC_CAPTURE:
        if (VideoCore::IsRenderDocLoaded()) {
            VideoCore::TriggerCapture();
        } else {
            VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
        }
        break;
    case SDL_EVENT_SCREENSHOT_WITH_OVERLAYS:
        VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
        break;
    default:
        break;
    }
}

void WindowSDL::InitTimers() {
    for (int i = 0; i < 4; ++i) {
        SDL_AddTimer(4, &PollController, controllers[i]);
    }
    SDL_AddTimer(33, Input::MousePolling, (void*)controllers[0]);
    if (Core::MouseAim::Enabled()) {
        // Native mouse camera: the mouse is captured from the start (F7 toggles). If the camera
        // hook was refused for this game build, it drives the right stick instead.
        Input::ToggleMouseModeTo(Input::MouseMode::Joystick);
        SDL_SetWindowRelativeMouseMode(window, true);
    }
}

void WindowSDL::RequestKeyboard() {
    if (keyboard_grab == 0) {
        SDL_RunOnMainThread(
            [](void* userdata) { SDL_StartTextInput(static_cast<SDL_Window*>(userdata)); }, window,
            true);
    }
    keyboard_grab++;
}

void WindowSDL::ReleaseKeyboard() {
    ASSERT(keyboard_grab > 0);
    keyboard_grab--;
    if (keyboard_grab == 0) {
        SDL_RunOnMainThread(
            [](void* userdata) { SDL_StopTextInput(static_cast<SDL_Window*>(userdata)); }, window,
            true);
    }
}

void WindowSDL::OnResize() {
    SDL_GetWindowSizeInPixels(window, &width, &height);
    ImGui::Core::OnResize();
}

Uint32 wheelOffCallback(void* og_event, Uint32 timer_id, Uint32 interval) {
    SDL_Event off_event = *(SDL_Event*)og_event;
    off_event.type = SDL_EVENT_MOUSE_WHEEL_OFF;
    SDL_PushEvent(&off_event);
    delete (SDL_Event*)og_event;
    return 0;
}

void WindowSDL::OnKeyboardMouseInput(const SDL_Event* event) {
    using Libraries::Pad::OrbisPadButtonDataOffset;

    // get the event's id, if it's keyup or keydown
    const bool input_down = event->type == SDL_EVENT_KEY_DOWN ||
                            event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                            event->type == SDL_EVENT_MOUSE_WHEEL;
    Input::InputEvent input_event = Input::InputBinding::GetInputEventFromSDLEvent(*event);

    // if it's a wheel event, make a timer that turns it off after a set time
    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        const SDL_Event* copy = new SDL_Event(*event);
        SDL_AddTimer(33, wheelOffCallback, (void*)copy);
    }

    // add/remove it from the list
    bool inputs_changed = Input::UpdatePressedKeys(input_event);

    // update bindings
    if (inputs_changed) {
        Input::ActivateOutputsFromInputs();
    }
}

void WindowSDL::OnGamepadEvent(const SDL_Event* event) {
    bool input_down = event->type == SDL_EVENT_GAMEPAD_AXIS_MOTION ||
                      event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    Input::InputEvent input_event = Input::InputBinding::GetInputEventFromSDLEvent(*event);

    // the touchpad button shouldn't be rebound to anything else,
    // as it would break the entire touchpad handling
    // You can still bind other things to it though
    if (event->gbutton.button == SDL_GAMEPAD_BUTTON_TOUCHPAD) {
        controllers[controllers.GetGamepadIndexFromJoystickId(event->gbutton.which)]->Button(
            OrbisPadButtonDataOffset::TouchPad, input_down);
        return;
    }

    u8 gamepad;

    switch (event->type) {
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        switch ((SDL_SensorType)event->gsensor.sensor) {
        case SDL_SENSOR_GYRO:
            gamepad = controllers.GetGamepadIndexFromJoystickId(event->gsensor.which);
            if (gamepad < 5) {
                controllers[gamepad]->UpdateGyro(event->gsensor.data);
            }
            break;
        case SDL_SENSOR_ACCEL:
            gamepad = controllers.GetGamepadIndexFromJoystickId(event->gsensor.which);
            if (gamepad < 5) {
                controllers[gamepad]->UpdateAcceleration(event->gsensor.data);
            }
            break;
        default:
            break;
        }
        return;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        controllers[controllers.GetGamepadIndexFromJoystickId(event->gtouchpad.which)]
            ->SetTouchpadState(event->gtouchpad.finger,
                               event->type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP, event->gtouchpad.x,
                               event->gtouchpad.y);
        return;
    default:
        break;
    }

    // add/remove it from the list
    bool inputs_changed = Input::UpdatePressedKeys(input_event);

    if (inputs_changed) {
        // update bindings
        Input::ActivateOutputsFromInputs();
    }
}

#ifndef __APPLE__
void SetWindowIcon(SDL_Window* window, const std::vector<u8>& png) {
    int imageWidth = 0;
    int imageHeight = 0;
    constexpr int numChannels = 4;
    unsigned char* imageData = stbi_load_from_memory(png.data(), png.size(), &imageWidth,
                                                     &imageHeight, nullptr, numChannels);
    if (imageData == nullptr) {
        LOG_ERROR(Core, "Failed to load window icon image: {}", stbi_failure_reason());
        return;
    }
    SCOPE_EXIT {
        stbi_image_free(imageData);
    };

    SDL_Surface* surface = SDL_CreateSurfaceFrom(imageWidth, imageHeight, SDL_PIXELFORMAT_RGBA32,
                                                 imageData, imageWidth * numChannels);
    if (surface == nullptr) {
        LOG_ERROR(Core, "Failed to create SDL surface for window icon: {}", SDL_GetError());
    }
    if (!SDL_SetWindowIcon(window, surface)) {
        LOG_ERROR(Core, "Failed to set SDL window icon: {}", SDL_GetError());
    }
    SDL_DestroySurface(surface);
}
#endif

void SetDefaultWindowIcon(SDL_Window* window) {
    const auto resource = cmrc::res::get_filesystem();
    const auto file = resource.open("src/resources/shadps4.png");
    const std::vector<u8> texData = std::vector<u8>(file.begin(), file.end());
    SetWindowIcon(window, texData);
}

} // namespace Frontend
