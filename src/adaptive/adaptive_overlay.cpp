// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The F1 window: what the optimizer sees, what it did and why, and the user's controls.

#include <atomic>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "adaptive/emulator_glue.h"
#include "adaptive/glue_internal.h"
#include "core/mouse_aim.h"
#include "imgui/imgui_layer.h"
#include "imgui/notice_layer.h"

namespace Adaptive::Glue {

namespace {

std::atomic<bool> g_visible{false};
bool g_registered = false;

const char* ModeHelp(Mode m) {
    switch (m) {
    case Mode::Observe:
        return "Measures only. Nothing is ever changed.";
    case Mode::Manual:
        return "Measures and recommends. You apply changes yourself (buttons below).";
    case Mode::Automatic:
        return "Applies safe live changes during gameplay, keeps only measured wins, and reuses "
               "values learned in earlier sessions.";
    case Mode::Calibration:
        return "Also tests one start-up setting per launch. After the game loads, leave it idle "
               "on the title screen (or a quiet spot) for 2 minutes, then quit normally.";
    }
    return "";
}

class AdaptiveLayer final : public ImGui::Layer {
public:
    void Draw() override {
        if (!g_visible.load(std::memory_order_relaxed)) {
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
        bool open = true;
        if (!ImGui::Begin("Second Son Adaptive (F1)", &open, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::End();
            return;
        }
        if (!open) {
            g_visible.store(false, std::memory_order_relaxed);
        }
        ImGui::TextUnformatted(("Mouse camera: " + Core::MouseAim::StatusText()).c_str());
        CentralBrain* brain = BrainForOverlay();
        if (!brain) {
            ImGui::TextWrapped("The optimizer is turned off (enabled=0 in user/adaptive/"
                               "settings.ini or SS_ADAPTIVE=0). The emulator runs with its "
                               "built-in values.");
            ImGui::End();
            return;
        }
        const StatusView st = brain->Status();
        UserSettings& settings = SettingsForOverlay();

        ImGui::SeparatorText("Mode");
        int mode = static_cast<int>(st.mode);
        bool changed = false;
        changed |= ImGui::RadioButton("Observe", &mode, static_cast<int>(Mode::Observe));
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Manual", &mode, static_cast<int>(Mode::Manual));
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Automatic", &mode, static_cast<int>(Mode::Automatic));
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Calibration", &mode, static_cast<int>(Mode::Calibration));
        if (changed) {
            const Mode m = static_cast<Mode>(mode);
            brain->RequestMode(m);
            settings.mode = m;
            SaveSettingsFromOverlay();
            if (m == Mode::Calibration) {
                Overlay::Notice::Show("Calibration starts with the next launch", 4.0);
            }
        }
        ImGui::TextWrapped("%s", ModeHelp(st.mode));
        if (st.kill_switch) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1),
                               "Automatic changes stopped for this session after repeated "
                               "rollbacks.");
        }

        ImGui::SeparatorText("Now");
        const Snapshot& s = st.snapshot;
        ImGui::Text("%.1f fps | frame p50 %.1f ms, p99 %.1f ms | %.1f hitches/min", s.fps,
                    s.frame_ms_p50, s.frame_ms_p99, s.hitches_per_minute);
        if (s.resources.vram_budget_bytes > 0) {
            ImGui::Text("VRAM %llu / %llu MB",
                        static_cast<unsigned long long>(s.resources.vram_used_bytes >> 20),
                        static_cast<unsigned long long>(s.resources.vram_budget_bytes >> 20));
        }
        ImGui::Text("Limit: %s (%.0f%%)%s", std::string(ToString(s.bottleneck)).c_str(),
                    s.bottleneck_confidence * 100.0, s.in_gameplay ? "" : " - not in gameplay");
        ImGui::Text("Scene: %s%s", std::string(ToString(s.scene)).c_str(),
                    s.gpu_busy_ms_p50 > 0.0
                        ? (" | graphics card busy " + std::to_string(static_cast<int>(
                                                          s.gpu_busy_ms_p50 + 0.5)) +
                           " ms a frame")
                              .c_str()
                        : "");
        if (st.scene_switches > 0) {
            ImGui::Text("Scene switches: %zu, worst freeze %.0f ms", st.scene_switches,
                        st.worst_switch_ms);
        }
        if (!st.fps_target.empty()) {
            ImGui::TextWrapped("Target: %s", st.fps_target.c_str());
        }
        if (st.evaluation) {
            const auto& e = *st.evaluation;
            ImGui::Text("Measuring %s %lld -> %lld: block %u/%u, %zu + %zu seconds", e.knob.c_str(),
                        static_cast<long long>(e.old_value), static_cast<long long>(e.new_value),
                        e.block + 1, e.blocks, e.seconds_a, e.seconds_b);
        }
        ImGui::Text("Start-up test: %s", st.calibration.c_str());

        if (!st.recommendations.empty()) {
            ImGui::SeparatorText("Recommendations");
            for (const auto& r : st.recommendations) {
                ImGui::PushID(r.knob.c_str());
                ImGui::TextWrapped("%s: %lld -> %lld - %s", r.knob.c_str(),
                                   static_cast<long long>(r.current),
                                   static_cast<long long>(r.value), r.reason.c_str());
                if (r.scope != Scope::Never) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(r.scope == Scope::Live ? "Try it" : "Test next launch")) {
                        brain->RequestApply(r.knob, r.value);
                    }
                } else if (r.knob == KnobId::PerfMode) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Use next launch")) {
                        auto patches = Glue::GamePatches();
                        patches.perf_mode = static_cast<std::uint32_t>(r.value);
                        Glue::SetGamePatches(patches);
                        Overlay::Notice::Show(std::string("Performance mode ") +
                                              (r.value == 1 ? "low" : "office") +
                                              " from the next launch (F3 > Graphics to change)");
                    }
                } else if (r.knob == KnobId::FrameLimit) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Set it")) {
                        Glue::SetFrameLimit(static_cast<std::uint32_t>(r.value));
                        Overlay::Notice::Show("Frame rate limit: " + std::to_string(r.value) +
                                              " fps (F3 > Graphics to change)");
                    }
                } else {
                    ImGui::TextDisabled("  (an emulator setting - change it in F3 settings)");
                }
                ImGui::PopID();
            }
        }

        if (ImGui::CollapsingHeader("Second Son game patches")) {
            const auto notes = Glue::GamePatchNotes();
            if (notes.empty()) {
                ImGui::TextDisabled("none this launch");
            }
            for (const auto& n : notes) {
                ImGui::BulletText("%s", n.c_str());
            }
            ImGui::TextDisabled("Change them in F3 > Graphics (they apply at the next launch).");
        }

        if (ImGui::CollapsingHeader("Mini-brains")) {
            for (const auto& [name, text] : st.brains) {
                ImGui::BulletText("%s: %s", name.c_str(), text.c_str());
            }
        }
        if (ImGui::CollapsingHeader("Values in effect")) {
            for (const auto& [k, v] : st.knob_values) {
                ImGui::BulletText("%s = %lld", k.c_str(), static_cast<long long>(v));
            }
        }
        if (ImGui::CollapsingHeader("Recent decisions")) {
            for (auto it = st.recent.rbegin(); it != st.recent.rend(); ++it) {
                ImGui::TextWrapped("%s %s %lld->%lld: %s [%s] %s", it->proposer.c_str(),
                                   it->knob.c_str(), static_cast<long long>(it->from),
                                   static_cast<long long>(it->to),
                                   std::string(ToString(it->decision.verdict)).c_str(),
                                   it->decision.rule.c_str(), it->decision.detail.c_str());
            }
        }
        if (ImGui::CollapsingHeader("Notes")) {
            for (const auto& n : st.notes) {
                ImGui::BulletText("%s", n.c_str());
            }
        }

        ImGui::SeparatorText("Safety");
        if (ImGui::Button("Restore emulator defaults now")) {
            brain->RequestRestoreDefaults();
        }
        ImGui::SameLine();
        static bool confirm_forget = false;
        if (!confirm_forget) {
            if (ImGui::Button("Forget what was learned...")) {
                confirm_forget = true;
            }
        } else {
            if (ImGui::Button("Yes, forget it")) {
                brain->RequestForgetProfile();
                confirm_forget = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                confirm_forget = false;
            }
        }
        ImGui::TextDisabled("optimizer cost %.3f ms per tick, %llu ticks, %llu samples dropped",
                            st.brain_ms_per_tick, static_cast<unsigned long long>(st.ticks),
                            static_cast<unsigned long long>(st.dropped_frames));
        ImGui::TextDisabled("Session report: user/adaptive/latest-report.txt (written on exit)");
        ImGui::End();
    }
};

AdaptiveLayer g_layer;

} // namespace

void ToggleOverlay() {
    if (!SettingsForOverlay().overlay) {
        Overlay::Notice::Show("The optimizer window is turned off (overlay=0 in settings.ini)");
        return;
    }
    g_visible.store(!g_visible.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

bool OverlayVisible() {
    return g_visible.load(std::memory_order_relaxed);
}

void RegisterOverlay() {
    if (!g_registered) {
        g_registered = true;
        ImGui::Layer::AddLayer(&g_layer);
    }
}

} // namespace Adaptive::Glue
