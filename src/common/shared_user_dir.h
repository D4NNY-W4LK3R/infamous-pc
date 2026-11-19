// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Second Son Adaptive: one data folder for every build.
//
// Each build is a zip extracted into its own folder ("SecondSonAdaptive-windows-x64",
// "... (1)", "... (2)"), and each folder used to get its own "user" folder: its own saves, shader
// cache, settings and optimizer knowledge. Starting a new build therefore looked like lost saves,
// stuttered while every shader was compiled again, and made the optimizer start over.
//
// The data now lives next to the build folders, in "<parent>/SecondSonAdaptive-data/user". The
// first launch that finds no such folder copies the most recently used "user" folder of this
// build or of a sibling build folder into it (copying, never moving or deleting: the old folders
// stay as they are). A file "portable.txt" next to the emulator, or SS_PORTABLE_USER=1, keeps the
// old behaviour (a "user" folder next to the emulator).

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace Common::FS {

struct SharedUserDirResult {
    std::filesystem::path user_dir;    ///< the folder to use
    std::filesystem::path copied_from; ///< set when this launch created it from an old folder
    std::string note;                  ///< one line for the log
};

/// Chooses (and on first use creates) the shared user folder for an emulator in exe_dir.
/// Returns nullopt when the portable layout is asked for or the shared folder cannot be made.
std::optional<SharedUserDirResult> ChooseSharedUserDir(const std::filesystem::path& exe_dir,
                                                       bool portable_requested);

/// The note of the choice made at startup (for the log, which starts after the choice).
const std::string& SharedUserDirNote();
void SetSharedUserDirNote(std::string note);

} // namespace Common::FS
