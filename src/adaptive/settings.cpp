// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

#include "adaptive/settings.h"

namespace Adaptive {

namespace {

std::string Trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::optional<bool> ParseBool(const std::string& v) {
    if (v == "1" || v == "true" || v == "on" || v == "yes") {
        return true;
    }
    if (v == "0" || v == "false" || v == "off" || v == "no") {
        return false;
    }
    return std::nullopt;
}

} // namespace

UserSettings ParseSettings(const std::string& text) {
    UserSettings s;
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            s.warnings.push_back("line " + std::to_string(n) + ": no '='");
            continue;
        }
        std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1));
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == "mode") {
            if (const auto m = ParseMode(value)) {
                s.mode = *m;
            } else {
                s.warnings.push_back("mode '" + value + "' unknown; kept " +
                                     std::string(ToString(s.mode)));
            }
        } else if (key == "enabled" || key == "overlay" || key == "calibrate_on_battery") {
            const auto b = ParseBool(value);
            if (!b) {
                s.warnings.push_back(key + " '" + value + "' is not 0/1");
                continue;
            }
            (key == "enabled" ? s.enabled : key == "overlay" ? s.overlay : s.calibrate_on_battery) =
                *b;
        } else if (key == "tick_ms") {
            std::uint32_t v = 0;
            const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
            if (r.ec != std::errc{} || v < 250 || v > 10000) {
                s.warnings.push_back("tick_ms must be 250..10000");
                continue;
            }
            s.tick_ms = v;
        } else {
            s.warnings.push_back("unknown setting '" + key + "'");
        }
    }
    return s;
}

std::string SerializeSettings(const UserSettings& s) {
    std::ostringstream o;
    o << "# Second Son Adaptive optimizer settings. Changed from the F9 window, or edit here.\n"
         "# mode: observe (measure only) | manual (recommend, you apply) |\n"
         "#       automatic (apply safe, measured live changes during gameplay) |\n"
         "#       calibration (also test one start-up setting per launch; leave the game idle\n"
         "#       on the title screen for 2 minutes after it loads)\n";
    o << "mode = " << ToString(s.mode) << '\n';
    o << "enabled = " << (s.enabled ? 1 : 0) << '\n';
    o << "overlay = " << (s.overlay ? 1 : 0) << '\n';
    o << "calibrate_on_battery = " << (s.calibrate_on_battery ? 1 : 0) << '\n';
    o << "tick_ms = " << s.tick_ms << '\n';
    return o.str();
}

UserSettings LoadSettings(const std::filesystem::path& file,
                          const std::function<std::optional<std::string>(const std::string&)>& env) {
    UserSettings s;
    if (std::ifstream in{file, std::ios::binary}) {
        std::ostringstream ss;
        ss << in.rdbuf();
        s = ParseSettings(ss.str());
    }
    if (const auto v = env("SS_ADAPTIVE")) {
        if (const auto b = ParseBool(*v)) {
            s.enabled = *b;
        }
    }
    if (const auto v = env("SS_ADAPTIVE_MODE")) {
        if (const auto m = ParseMode(*v)) {
            s.mode = *m;
        } else {
            s.warnings.push_back("SS_ADAPTIVE_MODE '" + *v + "' unknown");
        }
    }
    return s;
}

bool SaveSettings(const std::filesystem::path& file, const UserSettings& s) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto tmp = std::filesystem::path(file).concat(".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << SerializeSettings(s);
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

} // namespace Adaptive
