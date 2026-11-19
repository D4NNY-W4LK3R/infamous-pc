// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

#include "adaptive/quality_ladder.h"
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
        } else if (key == "enabled" || key == "overlay" || key == "calibrate_on_battery" ||
                   key == "controls_hint") {
            const auto b = ParseBool(value);
            if (!b) {
                s.warnings.push_back(key + " '" + value + "' is not 0/1");
                continue;
            }
            (key == "enabled"         ? s.enabled
             : key == "overlay"       ? s.overlay
             : key == "controls_hint" ? s.controls_hint
                                      : s.calibrate_on_battery) = *b;
        } else if (key == "tick_ms") {
            std::uint32_t v = 0;
            const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
            if (r.ec != std::errc{} || v < 250 || v > 10000) {
                s.warnings.push_back("tick_ms must be 250..10000");
                continue;
            }
            s.tick_ms = v;
        } else if (key == "perf_mode") {
            std::string v = value;
            std::transform(v.begin(), v.end(), v.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (v == "off" || v == "0") {
                s.perf_mode = 0;
            } else if (v == "low" || v == "1") {
                s.perf_mode = 1;
            } else if (v == "office" || v == "2") {
                s.perf_mode = 2;
            } else {
                s.warnings.push_back("perf_mode must be off, low or office");
            }
        } else if (key == "fps_target") {
            std::uint32_t v = 0;
            const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
            if (r.ec != std::errc{} || (v != 0 && (v < 15 || v > 240))) {
                s.warnings.push_back("fps_target must be 0 (off) or 15..240");
                continue;
            }
            s.fps_target = v;
        } else if (key == "auto_perf_max") {
            const auto parsed = ParseSettings("perf_mode = " + value + "\n");
            if (!parsed.warnings.empty()) {
                s.warnings.push_back("auto_perf_max must be off, low or office");
                continue;
            }
            s.auto_perf_max = parsed.perf_mode;
        } else if (key == "render_height" || key == "auto_render_min") {
            std::uint32_t v = 0;
            const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
            if (r.ec != std::errc{} || (v != 0 && NormalizeRenderHeight(v) != v)) {
                s.warnings.push_back(key + " must be 1080, 1008, 936, 864, 792, 720 or 648");
                continue;
            }
            (key == "render_height" ? s.render_height : s.auto_render_min) =
                NormalizeRenderHeight(v);
        } else if (key == "particle_late_read" || key == "particle_light_one_submit") {
            const auto b = ParseBool(value);
            if (!b) {
                s.warnings.push_back(key + " '" + value + "' is not 0/1");
                continue;
            }
            (key == "particle_late_read" ? s.particle_late_read : s.particle_light_one_submit) = *b;
        } else if (key == "frame_limit") {
            std::uint32_t v = 0;
            const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
            if (r.ec != std::errc{} || (v != 0 && (v < 15 || v > 240))) {
                s.warnings.push_back("frame_limit must be 0 (no limit) or 15..240");
                continue;
            }
            s.frame_limit = v;
        } else {
            s.warnings.push_back("unknown setting '" + key + "'");
        }
    }
    return s;
}

std::string SerializeSettings(const UserSettings& s) {
    std::ostringstream o;
    o << "# Second Son Adaptive settings. Changed from the F1 window / Home key, or edit here.\n"
         "# mode: observe (measure only) | manual (recommend, you apply) |\n"
         "#       automatic (apply safe, measured live changes during gameplay) |\n"
         "#       calibration (also test one start-up setting per launch; leave the game idle\n"
         "#       on the title screen for 2 minutes after it loads)\n";
    o << "mode = " << ToString(s.mode) << '\n';
    o << "enabled = " << (s.enabled ? 1 : 0) << '\n';
    o << "overlay = " << (s.overlay ? 1 : 0) << '\n';
    o << "controls_hint = " << (s.controls_hint ? 1 : 0) << '\n';
    o << "calibrate_on_battery = " << (s.calibrate_on_battery ? 1 : 0) << '\n';
    o << "tick_ms = " << s.tick_ms << '\n';
    o << "# most frames per second shown; 0 = no limit (F3 settings > Graphics)\n";
    o << "frame_limit = " << s.frame_limit << '\n';
    o << "# Second Son game patches (update 01.07 only; F3 settings > Graphics; next launch)\n";
    o << "# perf_mode: off | low | office - the game's own lower quality levels (less detail far\n"
         "#   away, one shadow layer less, lower-resolution particles, fewer high-res textures)\n";
    o << "perf_mode = " << (s.perf_mode == 1 ? "low" : s.perf_mode == 2 ? "office" : "off") << '\n';
    o << "particle_late_read = " << (s.particle_late_read ? 1 : 0) << '\n';
    o << "particle_light_one_submit = " << (s.particle_light_one_submit ? 1 : 0) << '\n';
    o << "# frame-rate target: the optimizer picks the performance mode per launch, from\n"
         "# perf_mode (your choice) down to auto_perf_max (off | low | office); 0 = off\n";
    o << "fps_target = " << s.fps_target << '\n';
    o << "auto_perf_max = "
      << (s.auto_perf_max == 1 ? "low" : s.auto_perf_max == 2 ? "office" : "off") << '\n';
    o << "# render size: the game renders this many lines (16:9; 1080 = its own size) and the\n"
         "# picture is scaled to the window: 1080 | 1008 | 936 | 864 | 792 | 720 | 648 (next launch)\n";
    o << "render_height = " << s.render_height << '\n';
    o << "# the smallest render size the frame-rate target may use (1080 = never change it); it\n"
         "# goes performance mode low first, then smaller sizes, then office if allowed\n";
    o << "auto_render_min = " << s.auto_render_min << '\n';
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
    // The v1.1.1 fork's variable names for its game patches win over the file too.
    if (const auto v = env("SHADPS4_PERF_MODE")) {
        const auto parsed = ParseSettings("perf_mode = " + *v + "\n");
        if (parsed.warnings.empty()) {
            s.perf_mode = parsed.perf_mode;
        } else {
            s.warnings.push_back("SHADPS4_PERF_MODE '" + *v + "' must be off, low or office");
        }
    }
    if (const auto v = env("SHADPS4_PARTICLE_LATE_READ")) {
        if (const auto b = ParseBool(*v)) {
            s.particle_late_read = *b;
        }
    }
    if (const auto v = env("SHADPS4_PARTICLE_LIGHT_ONE_SUBMIT")) {
        if (const auto b = ParseBool(*v)) {
            s.particle_light_one_submit = *b;
        }
    }
    if (const auto v = env("SS_FRAME_LIMIT")) {
        std::uint32_t f = 0;
        const auto r = std::from_chars(v->data(), v->data() + v->size(), f);
        if (r.ec == std::errc{} && (f == 0 || (f >= 15 && f <= 240))) {
            s.frame_limit = f;
        } else {
            s.warnings.push_back("SS_FRAME_LIMIT '" + *v + "' must be 0 or 15..240");
        }
    }
    // v1.1.1's name for the render height.
    if (const auto v = env("SHADPS4_RENDER_HEIGHT")) {
        std::uint32_t h = 0;
        const auto r = std::from_chars(v->data(), v->data() + v->size(), h);
        if (r.ec == std::errc{} && NormalizeRenderHeight(h) == h) {
            s.render_height = h;
        } else {
            s.warnings.push_back("SHADPS4_RENDER_HEIGHT '" + *v +
                                 "' must be 1080, 1008, 936, 864, 792, 720 or 648");
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
