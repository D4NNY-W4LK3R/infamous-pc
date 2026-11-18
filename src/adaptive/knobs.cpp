// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <charconv>

#include "adaptive/knobs.h"

namespace Adaptive {

bool KnobRegistry::Register(KnobSpec spec, std::optional<Actuator> actuator, int vram_direction) {
    if (spec.id.empty() || knobs.contains(spec.id) || spec.min_value > spec.max_value ||
        spec.default_value < spec.min_value || spec.default_value > spec.max_value) {
        return false;
    }
    for (const auto c : spec.candidates) {
        if (c < spec.min_value || c > spec.max_value) {
            return false;
        }
    }
    if (!spec.candidates.empty() &&
        std::find(spec.candidates.begin(), spec.candidates.end(), spec.default_value) ==
            spec.candidates.end()) {
        return false;
    }
    if (spec.scope == Scope::Live && (!actuator || !actuator->apply || !actuator->read)) {
        return false;
    }
    if (spec.scope != Scope::Live && actuator) {
        return false;
    }
    KnobState st;
    st.value = spec.default_value;
    st.spec = std::move(spec);
    st.actuator = std::move(actuator);
    st.vram_direction = vram_direction;
    if (st.actuator) {
        st.value = st.actuator->read();
    }
    const std::string id = st.spec.id;
    knobs.emplace(id, std::move(st));
    return true;
}

KnobState* KnobRegistry::Find(const std::string& id) {
    const auto it = knobs.find(id);
    return it == knobs.end() ? nullptr : &it->second;
}

const KnobState* KnobRegistry::Find(const std::string& id) const {
    const auto it = knobs.find(id);
    return it == knobs.end() ? nullptr : &it->second;
}

std::vector<std::string> KnobRegistry::Ids() const {
    std::vector<std::string> ids;
    for (const auto& [id, st] : knobs) {
        ids.push_back(id);
    }
    return ids;
}

std::vector<std::string> KnobRegistry::IdsOf(Domain domain) const {
    std::vector<std::string> ids;
    for (const auto& [id, st] : knobs) {
        if (st.spec.domain == domain) {
            ids.push_back(id);
        }
    }
    return ids;
}

bool KnobRegistry::Set(const std::string& id, std::int64_t value, std::uint64_t now_ns) {
    KnobState* st = Find(id);
    if (!st || value < st->spec.min_value || value > st->spec.max_value) {
        return false;
    }
    if (st->spec.scope == Scope::Live) {
        st->actuator->apply(value);
        st->value = st->actuator->read();
    } else {
        st->value = value;
    }
    st->last_change_ns = now_ns;
    return st->value == value;
}

void KnobRegistry::ResetLiveToDefaults(std::uint64_t now_ns) {
    for (auto& [id, st] : knobs) {
        if (st.spec.scope == Scope::Live && st.value != st.spec.default_value) {
            st.actuator->apply(st.spec.default_value);
            st.value = st.actuator->read();
            st.last_change_ns = now_ns;
        }
    }
}

std::map<std::string, std::int64_t> KnobRegistry::ApplyLaunchValues(
    const std::map<std::string, std::int64_t>& chosen,
    const std::function<std::optional<std::string>(const std::string&)>& env_get,
    const std::function<void(const std::string&, const std::string&)>& env_set) {
    std::map<std::string, std::int64_t> applied;
    for (auto& [id, st] : knobs) {
        if (st.spec.scope != Scope::NextLaunch) {
            continue;
        }
        if (const auto existing = env_get(id)) {
            st.user_pinned = true;
            std::int64_t v = st.spec.default_value;
            const auto* b = existing->data();
            const auto r = std::from_chars(b, b + existing->size(), v);
            st.value = r.ec == std::errc{} ? v : st.spec.default_value;
            continue;
        }
        const auto it = chosen.find(id);
        if (it == chosen.end() || it->second == st.spec.default_value ||
            it->second < st.spec.min_value || it->second > st.spec.max_value) {
            st.value = st.spec.default_value;
            continue;
        }
        env_set(id, std::to_string(it->second));
        st.value = it->second;
        applied[id] = it->second;
    }
    return applied;
}

} // namespace Adaptive
