// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "adaptive/brain.h"
#include "adaptive/settings.h"

// Shared between emulator_glue.cpp and adaptive_overlay.cpp only.
namespace Adaptive::Glue {
CentralBrain* BrainForOverlay(); ///< null when the analyser is disabled
UserSettings& SettingsForOverlay();
void SaveSettingsFromOverlay();
} // namespace Adaptive::Glue
