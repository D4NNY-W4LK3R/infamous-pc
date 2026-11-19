// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// A small picture of the PS4 touchpad at the bottom of the screen, shown while anything touches
// it (the touch grip, Delete's touch mode, the touchpad keys and swipes): where each finger is
// and whether the pad is pressed down, so a slide can be aimed at the mark the game shows.
namespace Overlay::Touchpad {

/// Called by the presenter once ImGui is up.
void Register();

} // namespace Overlay::Touchpad
