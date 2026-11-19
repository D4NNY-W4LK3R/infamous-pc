// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>

#include "input/touch_grip.h"

namespace Input {

void TouchGrip::Begin(double now) {
    if (gripping) {
        return;
    }
    gripping = true;
    begin_time = now;
    end_time = -1.0;
    fx = 0.5f;
    fy = 0.5f;
    travel = 0.0f;
    slid = false;
    second = false;
    used_second = false;
    press = false;
}

void TouchGrip::Move(float dx_px, float dy_px) {
    if (!gripping || cfg.px_per_pad_width <= 0.0f) {
        return;
    }
    fx = std::clamp(fx + dx_px / cfg.px_per_pad_width, 0.0f, 1.0f);
    fy = std::clamp(fy + dy_px / (cfg.px_per_pad_width * cfg.pad_aspect), 0.0f, 1.0f);
    // Distance from the start in pad widths (y scaled back to the same physical units).
    const float ddx = fx - 0.5f;
    const float ddy = (fy - 0.5f) * cfg.pad_aspect;
    travel = std::max(travel, std::sqrt(ddx * ddx + ddy * ddy));
    if (travel > cfg.drag_threshold) {
        slid = true;
    }
}

void TouchGrip::SetSecondFinger(bool on) {
    if (!gripping) {
        return;
    }
    second = on;
    used_second = used_second || on;
}

void TouchGrip::SetPress(bool on) {
    if (gripping) {
        press = on;
    }
}

PadTouch TouchGrip::Current() const {
    PadTouch out{};
    out.down[0] = true;
    if (second) {
        // A thumb on each half, centred on the finger: the pair moves with the mouse.
        const float half = cfg.thumb_spread * 0.5f;
        const float centre = std::clamp(fx, half, 1.0f - half);
        out.x[0] = centre - half;
        out.x[1] = centre + half;
        out.y[0] = out.y[1] = fy;
        out.down[1] = true;
    } else {
        out.x[0] = fx;
        out.y[0] = fy;
    }
    // Pressed on landing; a slide lifts the press (a swipe is a touch, not a click) unless the
    // left button presses it again.
    out.click = press || !slid;
    return out;
}

void TouchGrip::End(double now) {
    if (!gripping) {
        return;
    }
    final_out = Current();
    gripping = false;
    end_time = now;
    last.seconds = now - begin_time;
    last.dx = fx - 0.5f;
    last.dy = fy - 0.5f;
    last.travel = travel;
    last.slid = slid;
    last.two_fingers = used_second;
    last.pressed_at_end = final_out.click;
}

bool TouchGrip::Busy(double now) const {
    return gripping || (end_time >= 0.0 && now < begin_time + cfg.min_touch_s);
}

PadTouch TouchGrip::Output(double now) const {
    if (gripping) {
        return Current();
    }
    if (end_time >= 0.0 && now < begin_time + cfg.min_touch_s) {
        return final_out; // a quick click: the game still sees it for a few frames
    }
    return PadTouch{};
}

} // namespace Input
