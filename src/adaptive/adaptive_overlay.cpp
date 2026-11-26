// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The F1 window: what the performance analyser measures, the frame-rate target's choice, the
// recommendations, and the cheats.

#include <atomic>
#include <cstdio>
#include <string>

#include <imgui.h>

#include "adaptive/emulator_glue.h"
#include "adaptive/glue_internal.h"
#include "adaptive/settings.h"
#include "core/mouse_aim.h"
#include "core/second_son_cheats.h"
#include "imgui/imgui_layer.h"
#include "imgui/notice_layer.h"

namespace Adaptive::Glue {

namespace {

std::atomic<bool> g_visible{false};
bool g_registered = false;

/// F1 > Cheats (build 52): each cheat switches at once; "keep on" has it on at every launch.
void DrawCheats() {
    namespace Cheats = Core::SecondSonCheats;
    if (!ImGui::CollapsingHeader("Cheats")) {
        return;
    }
    if (Cheats::GetState(Cheats::Id::Energy) == Cheats::State::NotLoaded) {
        ImGui::TextDisabled("Available once the game is running.");
        return;
    }
    const std::uint32_t kept = Glue::CheatsKeptOn();
    for (std::size_t i = 0; i < Cheats::NumCheats; ++i) {
        const auto id = static_cast<Cheats::Id>(i);
        const auto state = Cheats::GetState(id);
        const bool available = state != Cheats::State::NotAvailable;
        ImGui::PushID(static_cast<int>(i));
        ImGui::BeginDisabled(!available);
        if (id == Cheats::Id::SmokeDash) {
            float scale = Cheats::SmokeScale();
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::SliderFloat(Cheats::Name(id), &scale, 1.0f, 5.0f, "%.1fx")) {
                Cheats::SetSmokeScale(scale);
            }
            // Kept on, the length kept follows the slider once it is let go (at 1: not kept).
            if (ImGui::IsItemDeactivatedAfterEdit() && Glue::SmokeDashKept() > 1.0f) {
                Glue::SetSmokeDashKept(Cheats::SmokeScale());
            }
            ImGui::SameLine(330.0f);
            bool keep = Glue::SmokeDashKept() > 1.0f;
            if (ImGui::Checkbox("keep on", &keep)) {
                if (keep && Cheats::SmokeScale() <= 1.0f) {
                    Cheats::SetSmokeScale(3.0f);
                }
                Glue::SetSmokeDashKept(keep ? Cheats::SmokeScale() : 1.0f);
            }
        } else {
            bool on = state == Cheats::State::On;
            if (ImGui::Checkbox(Cheats::Name(id), &on)) {
                const auto after = Cheats::Set(id, on);
                if (!on) {
                    // Off is off at the next launch too.
                    Glue::SetCheatKeptOn(static_cast<unsigned>(i), false);
                }
                if (after == Cheats::State::NotAvailable) {
                    Overlay::Notice::Show(std::string(Cheats::Name(id)) +
                                          ": not available for this version");
                }
            }
            ImGui::SameLine(330.0f);
            bool keep = (kept >> i) & 1u;
            if (ImGui::Checkbox("keep on", &keep)) {
                Glue::SetCheatKeptOn(static_cast<unsigned>(i), keep);
                if (keep) {
                    Cheats::Set(id, true);
                }
            }
        }
        ImGui::EndDisabled();
        if (!available) {
            ImGui::SameLine();
            ImGui::TextDisabled("not available for this version");
        } else if (const char* note = Cheats::Note(id)) {
            ImGui::TextDisabled("  %s", note);
        }
        ImGui::PopID();
    }
    ImGui::TextDisabled("Without \"keep on\" every cheat starts off at each launch.");
}

class AdaptiveLayer final : public ImGui::Layer {
public:
    void Draw() override {
        if (!g_visible.load(std::memory_order_relaxed)) {
            return;
        }
        ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
        bool open = true;
        if (!ImGui::Begin("Second Son Adaptive - performance analyser (F1)", &open,
                          ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::End();
            return;
        }
        if (!open) {
            g_visible.store(false, std::memory_order_relaxed);
        }
        ImGui::TextUnformatted(("Mouse camera: " + Core::MouseAim::StatusText()).c_str());
        DrawCheats();
        CentralBrain* brain = BrainForOverlay();
        if (!brain) {
            ImGui::TextWrapped("The performance analyser is turned off (enabled=0 in "
                               "user/adaptive/settings.ini or SS_ADAPTIVE=0): no measurements, "
                               "no report, no frame-rate target.");
            ImGui::End();
            return;
        }
        const StatusView st = brain->Status();

        ImGui::TextWrapped("Measures every second and writes a session report on exit. It "
                           "changes nothing while you play; the frame-rate target (F3 > "
                           "Graphics) is the one thing it chooses, once per launch.");

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
        ImGui::SeparatorText("Frame-rate target");
        if (!st.fps_target.empty()) {
            ImGui::TextWrapped("%s", st.fps_target.c_str());
        } else {
            ImGui::TextDisabled("off (F3 > Graphics to set one and allow lower quality)");
        }
        if (!st.recommendations.empty()) {
            ImGui::SeparatorText("Recommendations");
            for (const auto& r : st.recommendations) {
                ImGui::PushID(r.knob.c_str());
                ImGui::TextWrapped("%s: %lld -> %lld - %s", r.knob.c_str(),
                                   static_cast<long long>(r.current),
                                   static_cast<long long>(r.value), r.reason.c_str());
                if (r.knob == KnobId::PerfMode) {
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

        if (ImGui::CollapsingHeader("Measurements by area")) {
            for (const auto& [name, text] : st.brains) {
                ImGui::BulletText("%s: %s", name.c_str(), text.c_str());
            }
        }
        if (ImGui::CollapsingHeader("Settings in effect")) {
            for (const auto& [k, v] : st.knob_values) {
                ImGui::BulletText("%s = %lld", k.c_str(), static_cast<long long>(v));
            }
        }
        if (ImGui::CollapsingHeader("Notes")) {
            for (const auto& n : st.notes) {
                ImGui::BulletText("%s", n.c_str());
            }
        }

        ImGui::Separator();
        static bool confirm_forget = false;
        if (!confirm_forget) {
            if (ImGui::Button("Forget the frame-rate target's choices...")) {
                confirm_forget = true;
            }
        } else {
            if (ImGui::Button("Yes, forget them")) {
                brain->RequestForgetProfile();
                confirm_forget = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                confirm_forget = false;
            }
        }
        ImGui::TextDisabled("analyser cost %.3f ms per tick, %llu ticks, %llu samples dropped",
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
        Overlay::Notice::Show("The analyser window is turned off (overlay=0 in settings.ini)");
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
