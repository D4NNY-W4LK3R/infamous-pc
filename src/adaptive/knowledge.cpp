// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include "adaptive/knowledge.h"

namespace Adaptive {

namespace {

constexpr std::string_view Magic = "SSADAPTIVE-KNOWLEDGE";

std::string Escape(const std::string& s) {
    std::string out;
    if (s.empty()) {
        return "%-";
    }
    for (const unsigned char c : s) {
        if (c <= 0x20 || c == '%' || c >= 0x7f) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

bool Unescape(const std::string& s, std::string& out) {
    out.clear();
    if (s == "%-") {
        return true;
    }
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size()) {
            return false;
        }
        char hex[3] = {s[i + 1], s[i + 2], 0};
        char* end = nullptr;
        const long v = std::strtol(hex, &end, 16);
        if (end != hex + 2) {
            return false;
        }
        out += static_cast<char>(v);
        i += 2;
    }
    return true;
}

std::optional<Outcome> ParseOutcome(std::string_view s) {
    for (int i = 0; i <= static_cast<int>(Outcome::Abandoned); ++i) {
        if (ToString(static_cast<Outcome>(i)) == s) {
            return static_cast<Outcome>(i);
        }
    }
    return std::nullopt;
}

std::string Num(double v) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

} // namespace

std::string_view ToString(LoadStatus status) {
    switch (status) {
    case LoadStatus::Fresh:
        return "fresh";
    case LoadStatus::Loaded:
        return "loaded";
    case LoadStatus::RecoveredBackup:
        return "recovered from backup";
    case LoadStatus::BothCorrupt:
        return "both copies corrupt, started fresh";
    case LoadStatus::SchemaChanged:
        return "schema changed, learned values reset";
    }
    return "?";
}

KnowledgeStore::KnowledgeStore(std::filesystem::path file_) : file{std::move(file_)} {}

std::string KnowledgeStore::Serialize(const Knowledge& k) {
    std::ostringstream o;
    o << Magic << ' ' << k.schema_version << '\n';
    for (const auto& [key, p] : k.profiles) {
        o << "profile " << Escape(key) << '\n';
        o << "fp.hardware " << Escape(p.fingerprint.hardware) << '\n';
        o << "fp.driver " << Escape(p.fingerprint.driver) << '\n';
        o << "fp.game " << Escape(p.fingerprint.game) << '\n';
        o << "fp.emulator " << Escape(p.fingerprint.emulator) << '\n';
        o << "sessions " << p.sessions << '\n';
        o << "revalidation " << Escape(p.revalidation_reason) << '\n';
        o << "bottleneck " << Escape(p.last_bottleneck) << '\n';
        o << "unclean " << p.unclean_sessions << '\n';
        for (const auto& [knob, v] : p.revalidate) {
            o << "revalidate " << Escape(knob) << ' ' << v << '\n';
        }
        for (const auto& [knob, v] : p.validated) {
            o << "validated " << Escape(knob) << ' ' << v << '\n';
        }
        for (const auto& [knob, values] : p.rejected) {
            for (const auto& [v, outcome] : values) {
                o << "rejected " << Escape(knob) << ' ' << v << ' ' << ToString(outcome) << '\n';
            }
        }
        for (std::size_t i = 0; i < p.baseline_seconds_ms.size(); ++i) {
            const double p99 =
                i < p.baseline_seconds_p99_ms.size() ? p.baseline_seconds_p99_ms[i] : 0.0;
            const std::uint64_t sig =
                i < p.baseline_signatures.size() ? p.baseline_signatures[i] : 0;
            o << "baseline " << Num(p.baseline_seconds_ms[i]) << ' ' << Num(p99) << ' ' << sig
              << '\n';
        }
        if (p.auto_perf_mode >= 0) {
            o << "autoperf " << p.auto_perf_mode << '\n';
        }
        if (p.partial) {
            const auto& e = *p.partial;
            o << "partial " << Escape(e.knob) << ' ' << e.old_value << ' ' << e.new_value << ' '
              << e.a_hitches << ' ' << e.b_hitches << ' ' << e.started_unix << ' ' << e.sessions
              << '\n';
            for (std::size_t i = 0; i < e.a_median.size(); ++i) {
                o << "partialsec A " << Num(e.a_median[i]) << ' '
                  << Num(i < e.a_p99.size() ? e.a_p99[i] : 0.0) << '\n';
            }
            for (std::size_t i = 0; i < e.b_median.size(); ++i) {
                o << "partialsec B " << Num(e.b_median[i]) << ' '
                  << Num(i < e.b_p99.size() ? e.b_p99[i] : 0.0) << '\n';
            }
        }
        for (const auto& e : p.history) {
            o << "history " << Escape(e.knob) << ' ' << e.baseline_value << ' ' << e.value << ' '
              << ToString(e.outcome) << ' ' << Num(e.median_before_ms) << ' '
              << Num(e.median_after_ms) << ' ' << Num(e.p99_before_ms) << ' '
              << Num(e.p99_after_ms) << ' ' << Num(e.hitches_before) << ' '
              << Num(e.hitches_after) << ' ' << e.seconds_before << ' ' << e.seconds_after << ' '
              << Num(e.p_value) << ' ' << e.unix_time << ' ' << Escape(e.driver) << ' '
              << Escape(e.method) << '\n';
        }
        o << "endprofile\n";
    }
    o << "session " << (k.session_open ? "open" : "closed") << ' ' << Escape(k.session_profile);
    if (k.session_experiment) {
        o << ' ' << Escape(k.session_experiment->knob) << ' '
          << k.session_experiment->baseline_value << ' ' << k.session_experiment->value;
    }
    o << '\n';
    for (const auto& [knob, v] : k.session_launch_values) {
        o << "launchvalue " << Escape(knob) << ' ' << v << '\n';
    }
    std::string body = o.str();
    body += "checksum " + Hex64(Fnv1a64(body.data(), body.size())) + "\n";
    return body;
}

namespace {
/// Schema < 3 kept one profile per emulator build. They are merged into one profile per hardware +
/// game: the most recently used one is the base (its learned values, baseline and fingerprint);
/// the others add their history, session counts and crash blocks (a value that crashed stays
/// blocked); their regressions are not carried over, since they were measured on older builds.
std::map<std::string, Profile> MergeIntoV3Keys(std::map<std::string, Profile> old,
                                               std::string& session_profile) {
    const auto latest = [](const Profile& p) {
        std::int64_t t = 0;
        for (const auto& e : p.history) {
            t = std::max(t, e.unix_time);
        }
        return t;
    };
    std::map<std::string, std::vector<std::pair<std::string, Profile>>> groups;
    for (auto& [key, p] : old) {
        groups[p.fingerprint.ProfileKey()].emplace_back(key, std::move(p));
    }
    std::map<std::string, Profile> out;
    for (auto& [new_key, members] : groups) {
        std::stable_sort(members.begin(), members.end(), [&](const auto& a, const auto& b) {
            const auto ta = latest(a.second), tb = latest(b.second);
            return ta != tb ? ta > tb : a.second.sessions > b.second.sessions;
        });
        Profile merged = members.front().second;
        for (std::size_t i = 1; i < members.size(); ++i) {
            const Profile& m = members[i].second;
            for (const auto& [knob, values] : m.rejected) {
                for (const auto& [v, outcome] : values) {
                    if (outcome == Outcome::Crashed) {
                        merged.rejected[knob][v] = Outcome::Crashed;
                    }
                }
            }
            merged.history.insert(merged.history.end(), m.history.begin(), m.history.end());
            merged.sessions += m.sessions;
            merged.unclean_sessions += m.unclean_sessions;
        }
        std::stable_sort(merged.history.begin(), merged.history.end(),
                         [](const auto& a, const auto& b) { return a.unix_time < b.unix_time; });
        if (merged.history.size() > Profile::MaxHistory) {
            merged.history.erase(merged.history.begin(),
                                 merged.history.end() -
                                     static_cast<std::ptrdiff_t>(Profile::MaxHistory));
        }
        for (const auto& [old_key, _] : members) {
            if (old_key == session_profile) {
                session_profile = new_key;
            }
        }
        out[new_key] = std::move(merged);
    }
    return out;
}
} // namespace

bool KnowledgeStore::Parse(const std::string& text, Knowledge& out, bool& schema_changed) {
    schema_changed = false;
    const auto cpos = text.rfind("checksum ");
    if (cpos == std::string::npos) {
        return false;
    }
    std::string stored = text.substr(cpos + 9);
    while (!stored.empty() && (stored.back() == '\n' || stored.back() == '\r')) {
        stored.pop_back();
    }
    if (stored != Hex64(Fnv1a64(text.data(), cpos))) {
        return false;
    }
    Knowledge k;
    std::istringstream in(text.substr(0, cpos));
    std::string line;
    if (!std::getline(in, line)) {
        return false;
    }
    {
        std::istringstream hl(line);
        std::string magic;
        std::uint32_t version = 0;
        if (!(hl >> magic >> version) || magic != Magic) {
            return false;
        }
        k.schema_version = version;
        if (version != KnowledgeSchemaVersion) {
            schema_changed = true;
        }
    }
    const std::uint32_t loaded_version = k.schema_version;
    Profile* cur = nullptr;
    std::string cur_key;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        std::string a, b;
        if (tag == "profile") {
            if (!(ls >> a) || !Unescape(a, cur_key)) {
                return false;
            }
            cur = &k.profiles[cur_key];
        } else if (tag == "endprofile") {
            cur = nullptr;
        } else if (tag.rfind("fp.", 0) == 0 && cur) {
            std::string v;
            if (!(ls >> a) || !Unescape(a, v)) {
                return false;
            }
            if (tag == "fp.hardware") {
                cur->fingerprint.hardware = v;
            } else if (tag == "fp.driver") {
                cur->fingerprint.driver = v;
            } else if (tag == "fp.game") {
                cur->fingerprint.game = v;
            } else if (tag == "fp.emulator") {
                cur->fingerprint.emulator = v;
            }
        } else if (tag == "sessions" && cur) {
            if (!(ls >> cur->sessions)) {
                return false;
            }
        } else if (tag == "revalidation" && cur) {
            if (!(ls >> a) || !Unescape(a, cur->revalidation_reason)) {
                return false;
            }
        } else if (tag == "bottleneck" && cur) {
            if (!(ls >> a) || !Unescape(a, cur->last_bottleneck)) {
                return false;
            }
        } else if (tag == "unclean" && cur) {
            if (!(ls >> cur->unclean_sessions)) {
                return false;
            }
        } else if (tag == "revalidate" && cur) {
            std::int64_t v = 0;
            std::string knob;
            if (!(ls >> a >> v) || !Unescape(a, knob)) {
                return false;
            }
            cur->revalidate[knob] = v;
        } else if (tag == "validated" && cur) {
            std::int64_t v = 0;
            std::string knob;
            if (!(ls >> a >> v) || !Unescape(a, knob)) {
                return false;
            }
            cur->validated[knob] = v;
        } else if (tag == "rejected" && cur) {
            std::int64_t v = 0;
            std::string knob;
            if (!(ls >> a >> v >> b) || !Unescape(a, knob)) {
                return false;
            }
            const auto o = ParseOutcome(b);
            if (!o) {
                return false;
            }
            cur->rejected[knob][v] = *o;
        } else if (tag == "baseline" && cur) {
            double m = 0.0, p = 0.0;
            if (!(ls >> m >> p)) {
                return false;
            }
            std::uint64_t sig = 0;
            if (!(ls >> sig)) {
                sig = 0; // schema 2 lines carry no signature
            }
            cur->baseline_seconds_ms.push_back(m);
            cur->baseline_seconds_p99_ms.push_back(p);
            cur->baseline_signatures.push_back(sig);
        } else if (tag == "autoperf" && cur) {
            if (!(ls >> cur->auto_perf_mode) || cur->auto_perf_mode < -1 ||
                cur->auto_perf_mode > 2) {
                return false;
            }
        } else if (tag == "partial" && cur) {
            PartialEvaluation e;
            if (!(ls >> a >> e.old_value >> e.new_value >> e.a_hitches >> e.b_hitches >>
                  e.started_unix >> e.sessions) ||
                !Unescape(a, e.knob)) {
                return false;
            }
            cur->partial = std::move(e);
        } else if (tag == "partialsec" && cur && cur->partial) {
            std::string side;
            double m = 0.0, p = 0.0;
            if (!(ls >> side >> m >> p) || (side != "A" && side != "B")) {
                return false;
            }
            auto& e = *cur->partial;
            (side == "A" ? e.a_median : e.b_median).push_back(m);
            (side == "A" ? e.a_p99 : e.b_p99).push_back(p);
        } else if (tag == "history" && cur) {
            ExperimentRecord e;
            std::string outcome, drv, method;
            if (!(ls >> a >> e.baseline_value >> e.value >> outcome >> e.median_before_ms >>
                  e.median_after_ms >> e.p99_before_ms >> e.p99_after_ms >> e.hitches_before >>
                  e.hitches_after >> e.seconds_before >> e.seconds_after >> e.p_value >>
                  e.unix_time >> drv >> method) ||
                !Unescape(a, e.knob) || !Unescape(drv, e.driver) || !Unescape(method, e.method)) {
                return false;
            }
            const auto o = ParseOutcome(outcome);
            if (!o) {
                return false;
            }
            e.outcome = *o;
            cur->history.push_back(std::move(e));
        } else if (tag == "session") {
            if (!(ls >> a >> b) || !Unescape(b, k.session_profile)) {
                return false;
            }
            k.session_open = a == "open";
            std::string knob_esc;
            PendingExperiment pe;
            if (ls >> knob_esc >> pe.baseline_value >> pe.value) {
                if (!Unescape(knob_esc, pe.knob)) {
                    return false;
                }
                k.session_experiment = pe;
            }
        } else if (tag == "launchvalue") {
            std::int64_t v = 0;
            std::string knob;
            if (!(ls >> a >> v) || !Unescape(a, knob)) {
                return false;
            }
            k.session_launch_values[knob] = v;
        } else {
            return false; // unknown line: refuse rather than guess
        }
    }
    if (schema_changed && loaded_version < 3) {
        k.profiles = MergeIntoV3Keys(std::move(k.profiles), k.session_profile);
    }
    if (schema_changed && loaded_version < 2) {
        // Keep history and crash knowledge (facts about what happened), drop learned values.
        for (auto& [key, p] : k.profiles) {
            for (const auto& [knob, v] : p.validated) {
                p.revalidate[knob] = v;
            }
            p.validated.clear();
            p.baseline_seconds_ms.clear();
            p.baseline_seconds_p99_ms.clear();
            p.revalidation_reason = "knowledge schema changed";
            // v1 could block a value after a single hitch second; give those another chance.
            for (auto& [knob, values] : p.rejected) {
                std::erase_if(values,
                              [](const auto& kv) { return kv.second == Outcome::Regressed; });
            }
            std::erase_if(p.rejected, [](const auto& kv) { return kv.second.empty(); });
        }
    }
    if (schema_changed) {
        k.schema_version = KnowledgeSchemaVersion;
    }
    out = std::move(k);
    return true;
}

namespace {
bool ReadFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}
} // namespace

LoadStatus KnowledgeStore::Load(Knowledge& out) {
    std::error_code ec;
    const auto bak = std::filesystem::path(file).concat(".bak");
    const bool main_exists = std::filesystem::exists(file, ec);
    const bool bak_exists = std::filesystem::exists(bak, ec);
    if (!main_exists && !bak_exists) {
        out = Knowledge{};
        return LoadStatus::Fresh;
    }
    std::string text;
    bool schema_changed = false;
    if (main_exists && ReadFile(file, text) && Parse(text, out, schema_changed)) {
        return schema_changed ? LoadStatus::SchemaChanged : LoadStatus::Loaded;
    }
    if (bak_exists && ReadFile(bak, text) && Parse(text, out, schema_changed)) {
        // Keep the bad main file for diagnosis, then restore the good copy.
        if (main_exists) {
            std::filesystem::rename(file, std::filesystem::path(file).concat(".corrupt"), ec);
        }
        return LoadStatus::RecoveredBackup;
    }
    if (main_exists) {
        std::filesystem::rename(file, std::filesystem::path(file).concat(".corrupt"), ec);
    }
    if (bak_exists) {
        std::filesystem::rename(bak, std::filesystem::path(bak).concat(".corrupt"), ec);
    }
    out = Knowledge{};
    return LoadStatus::BothCorrupt;
}

bool KnowledgeStore::Save(const Knowledge& k) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto tmp = std::filesystem::path(file).concat(".tmp");
    const auto bak = std::filesystem::path(file).concat(".bak");
    const std::string text = Serialize(k);
    {
        std::ofstream outf(tmp, std::ios::binary | std::ios::trunc);
        if (!outf) {
            return false;
        }
        outf.write(text.data(), static_cast<std::streamsize>(text.size()));
        outf.flush();
        if (!outf) {
            return false;
        }
    }
    // Verify what reached the disk before it replaces anything.
    std::string check;
    Knowledge parsed;
    bool sc = false;
    if (!ReadFile(tmp, check) || check != text || !Parse(check, parsed, sc)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    if (std::filesystem::exists(file, ec)) {
        // The current file becomes the last-known-good copy, but only if it is itself valid;
        // a corrupt main file must never overwrite a good backup.
        std::string cur;
        Knowledge cur_parsed;
        if (ReadFile(file, cur) && Parse(cur, cur_parsed, sc)) {
            std::filesystem::copy_file(file, bak, std::filesystem::copy_options::overwrite_existing,
                                       ec);
            if (ec) {
                std::filesystem::remove(tmp, ec);
                return false;
            }
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

Profile& SelectProfile(Knowledge& k, const Fingerprint& current, std::vector<std::string>& notes) {
    const std::string key = current.ProfileKey();
    auto [it, inserted] = k.profiles.try_emplace(key);
    Profile& p = it->second;
    if (inserted) {
        p.fingerprint = current;
        notes.push_back("new optimization profile " + key + " (no earlier results for this "
                        "hardware + game build)");
        // Mention what changed relative to the most recent other profile, if any.
        for (const auto& [other_key, other] : k.profiles) {
            if (other_key == key) {
                continue;
            }
            for (const auto& d : Differences(other.fingerprint, current)) {
                notes.push_back("differs from profile " + other_key + ": " + d);
            }
            break;
        }
        return p;
    }
    if (p.fingerprint.driver != current.driver) {
        notes.push_back("GPU driver changed (" + p.fingerprint.driver + " -> " + current.driver +
                        "): learned values are revalidated before use, values that only "
                        "regressed may be tried again, values that crashed stay blocked");
        for (auto& [knob, values] : p.rejected) {
            std::erase_if(values,
                          [](const auto& kv) { return kv.second == Outcome::Regressed; });
        }
        std::erase_if(p.rejected, [](const auto& kv) { return kv.second.empty(); });
        for (const auto& [knob, v] : p.validated) {
            p.revalidate[knob] = v;
        }
        p.validated.clear();
        p.revalidation_reason = "GPU driver changed";
        p.baseline_seconds_ms.clear();
        p.baseline_seconds_p99_ms.clear();
        p.baseline_signatures.clear();
        p.partial.reset();
        p.fingerprint.driver = current.driver;
    }
    if (p.fingerprint.emulator != current.emulator) {
        // An emulator update keeps everything learned, but a value that won on the old build is
        // re-checked before it is trusted again; what crashed or clearly lost stays blocked, and
        // an unfinished test starts over (its seconds were measured on the old build).
        notes.push_back("emulator build changed (" + p.fingerprint.emulator + " -> " +
                        current.emulator + "): " + std::to_string(p.validated.size()) +
                        " learned value(s) are re-checked before use; blocked values stay "
                        "blocked");
        for (const auto& [knob, v] : p.validated) {
            p.revalidate[knob] = v;
        }
        p.validated.clear();
        if (!p.revalidate.empty()) {
            p.revalidation_reason = "emulator build changed";
        }
        p.baseline_seconds_ms.clear();
        p.baseline_seconds_p99_ms.clear();
        p.baseline_signatures.clear();
        p.partial.reset();
        p.fingerprint.emulator = current.emulator;
    }
    return p;
}

} // namespace Adaptive
