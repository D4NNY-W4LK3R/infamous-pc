// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Touch grip: the mouse as a finger on the PS4 touchpad, anywhere in the game, with no mode to
// switch. Holding the grip button (middle mouse button or the back side button) puts a finger in
// the middle of the touchpad and presses the pad down; moving the mouse slides the finger the way
// a laptop touchpad would. Everything Second Son asks of the touchpad comes out of that one
// gesture:
//   tap                    - a touch with the pad pressed (absorbing a Core Relay)
//   hold still             - the pad stays pressed (draining smoke / neon / video / concrete)
//   move, then let go      - a swipe in any direction (doors, wreckage, crates)
//   move, then keep holding- swipe and hold (the Mobile Command core; the left button stays R2)
//   press, slide to a mark - the D.U.P. hand scanner
//   + right button         - a second finger, a thumb on each half (lifting the car, "Move car")
// SetPress() presses the pad down again during a slide (not bound to a mouse button: the left
// button stays R2 for the Mobile Command core).
// This file holds only the gesture logic, without SDL, so it can be tested on its own.

namespace Input {

struct PadTouch {
    bool down[2] = {false, false};
    float x[2] = {0.5f, 0.5f}; ///< 0..1 across the pad
    float y[2] = {0.5f, 0.5f}; ///< 0..1 down the pad
    bool click = false;        ///< the pad is pressed down
    bool operator==(const PadTouch&) const = default;
};

struct TouchGripConfig {
    /// Mouse pixels that move the finger across the whole width of the pad.
    float px_per_pad_width = 600.0f;
    /// The pad is 1920 x 942 on a DualShock 4: a pixel moves the finger as far down as across.
    float pad_aspect = 942.0f / 1920.0f;
    /// How far (in pad widths) the finger moves before a press turns into a slide.
    float drag_threshold = 0.04f;
    /// The shortest touch the game is shown, so a quick click is still seen at 30 fps.
    double min_touch_s = 0.12;
    /// Distance between the two thumbs (pad widths).
    float thumb_spread = 0.4f;
};

class TouchGrip {
public:
    explicit TouchGrip(TouchGripConfig config = {}) : cfg(config) {}

    /// The grip button went down. Ignored if already gripping.
    void Begin(double now);
    /// Relative mouse motion while gripping, in pixels.
    void Move(float dx_px, float dy_px);
    /// Right button while gripping: a second finger (a thumb on each half of the pad).
    void SetSecondFinger(bool on);
    /// Left button while gripping: press the pad down, even after the finger slid.
    void SetPress(bool on);
    /// The grip button went up. A very short touch is still shown for min_touch_s.
    void End(double now);

    /// What the touchpad shows now.
    PadTouch Output(double now) const;
    /// Between Begin and End.
    bool Gripping() const {
        return gripping;
    }
    /// Gripping, or still showing a short tap.
    bool Busy(double now) const;

    struct Summary {
        double seconds = 0.0;
        float dx = 0.0f, dy = 0.0f; ///< end minus start, pad widths / heights
        float travel = 0.0f;        ///< longest distance from the start, pad widths
        bool slid = false;
        bool two_fingers = false;
        bool pressed_at_end = false;
    };
    /// The last finished gesture (valid after End).
    const Summary& Last() const {
        return last;
    }

private:
    TouchGripConfig cfg;
    bool gripping = false;
    double begin_time = 0.0;
    double end_time = -1.0;
    float fx = 0.5f, fy = 0.5f; ///< finger 1
    float travel = 0.0f;
    bool slid = false;
    bool second = false;
    bool used_second = false;
    bool press = false;
    PadTouch final_out{}; ///< what a short tap keeps showing after End
    Summary last{};

    PadTouch Current() const;
};

} // namespace Input
