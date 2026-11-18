// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

// A short message at the top of the screen (mouse look on / off, mouse speed, optimizer notes).
// Safe to call from any thread, also before the renderer exists (then it is shown once it does).
namespace Overlay::Notice {

void Show(std::string text, double seconds = 2.5);
/// Called by the presenter once ImGui is up.
void Register();

} // namespace Overlay::Notice
