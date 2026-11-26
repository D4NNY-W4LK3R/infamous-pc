// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>
#include <vector>

#include "common/shared_user_dir.h"

namespace Common::FS {

namespace fs = std::filesystem;

namespace {

constexpr const char* DataFolderName = "SecondSonAdaptive-data";

/// When the player last used a user folder: its newest save, settings or optimizer file.
fs::file_time_type LastUse(const fs::path& user) {
    std::error_code ec;
    fs::file_time_type newest = fs::file_time_type::min();
    const auto consider = [&](const fs::path& p) {
        const auto t = fs::last_write_time(p, ec);
        if (!ec && t > newest) {
            newest = t;
        }
    };
    const fs::path home = user / "home";
    if (fs::is_directory(home, ec)) {
        for (auto it = fs::recursive_directory_iterator(home, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) &&
                it->path().string().find("savedata") != std::string::npos) {
                consider(it->path());
            }
        }
    }
    consider(user / "adaptive" / "knowledge.txt");
    consider(user / "adaptive" / "settings.ini");
    return newest;
}

bool LooksLikeEmulatorFolder(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(dir / "shadPS4.exe", ec) || fs::exists(dir / "shadps4", ec) ||
           fs::exists(dir / "shadPS4", ec);
}

} // namespace

std::optional<SharedUserDirResult> ChooseSharedUserDir(const fs::path& exe_dir,
                                                       bool portable_requested) {
    std::error_code ec;
    if (portable_requested || fs::exists(exe_dir / "portable.txt", ec)) {
        return std::nullopt;
    }
    const fs::path parent = exe_dir.parent_path();
    if (parent.empty() || parent == exe_dir) {
        return std::nullopt; // at the root of a drive: no place beside it
    }
    const fs::path root = parent / DataFolderName;
    const fs::path shared = root / "user";
    SharedUserDirResult r;
    r.user_dir = shared;
    if (fs::is_directory(shared, ec)) {
        r.note = "data folder " + shared.string() + " (shared by every build)";
        return r;
    }
    // First launch with the shared folder: adopt the most recently used old one.
    std::vector<fs::path> candidates;
    if (fs::is_directory(exe_dir / "user", ec)) {
        candidates.push_back(exe_dir / "user");
    }
    for (auto it = fs::directory_iterator(parent, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const fs::path dir = it->path();
        if (dir == exe_dir || dir == root || !it->is_directory(ec)) {
            continue;
        }
        if (LooksLikeEmulatorFolder(dir) && fs::is_directory(dir / "user", ec)) {
            candidates.push_back(dir / "user");
        }
    }
    std::optional<fs::path> best;
    fs::file_time_type best_time = fs::file_time_type::min();
    for (const auto& c : candidates) {
        const auto t = LastUse(c);
        if (!best || t > best_time) {
            best = c;
            best_time = t;
        }
    }
    fs::create_directories(root, ec);
    if (ec) {
        return std::nullopt;
    }
    if (best) {
        fs::copy(*best, shared, fs::copy_options::recursive | fs::copy_options::skip_existing,
                 ec);
        if (ec) {
            // A half copy is worse than none: leave it for the player to see, use the old way.
            return std::nullopt;
        }
        r.copied_from = *best;
        r.note = "data folder " + shared.string() + " created from " + best->string() +
                 " (copied; the old folder is unchanged)";
    } else {
        fs::create_directories(shared, ec);
        if (ec) {
            return std::nullopt;
        }
        r.note = "data folder " + shared.string() + " created (no earlier data found)";
    }
    std::ofstream readme(root / "README.txt");
    readme << "Second Son Adaptive keeps your saves, shader cache, settings and what the analyser\n"
              "learned here, in the folder \"user\". Every build you extract next to this folder\n"
              "uses it, so updating keeps everything.\n\n"
              "To give one build its own data instead, put an empty file named portable.txt\n"
              "next to its shadPS4.exe.\n";
    if (best) {
        readme << "\nCreated from: " << best->string() << "\n";
    }
    return r;
}

namespace {
std::string& Note() {
    static std::string note;
    return note;
}
} // namespace

const std::string& SharedUserDirNote() {
    return Note();
}

void SetSharedUserDirNote(std::string note) {
    Note() = std::move(note);
}

} // namespace Common::FS
