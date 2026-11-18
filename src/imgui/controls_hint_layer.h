// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The keyboard / mouse equivalents of the game's PlayStation button prompts, on screen.
namespace Overlay::ControlsHint {

/// Home key: show / hide (remembered in user/adaptive/settings.ini, controls_hint=).
void Toggle();
/// Called by the presenter once ImGui is up.
void Register();

} // namespace Overlay::ControlsHint
