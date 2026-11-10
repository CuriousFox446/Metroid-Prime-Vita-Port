// F1 overlay: The Performance tab. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include <aurora/imgui.h>
#include <imgui_internal.h>

#include <cstdio>


namespace PortDebug {

void DrawPerformanceTab() {
  ImGui::SeparatorText("Frame rate");
  const bool locked = BeginOriginalLocked();
  bool frameLimit = sFrameLimitEnabled;
  if (ImGui::Checkbox("60 FPS cap (target)", &frameLimit)) {
    SetFrameLimitEnabled(frameLimit);
    MarkDirty();
  }
  {
    // One past the top is "Unlimited" (saved as 0).
    ImGui::BeginDisabled(sFrameLimitEnabled);
    int cap = sFpsCap > 0 ? sFpsCap : kFpsCapMax + 1;
    if (ImGui::SliderInt("FPS cap", &cap, kFpsCapMin, kFpsCapMax + 1, cap > kFpsCapMax ? "Unlimited" : "%d FPS",
                         ImGuiSliderFlags_AlwaysClamp)) {
      sFpsCap = cap > kFpsCapMax ? 0 : ClampFpsCap(cap);
      ResetDynamicRes();
      MarkDirty();
    }
    ImGui::EndDisabled();
    ItemHelp("The most frames drawn a second with the 60 FPS cap off. Vsync still limits it to the "
             "display's refresh rate.");
  }
  // Both, because they answer different questions. Throughput is what the
  // machine produces once the pacing wait is excluded; presented is what
  // reaches the screen. A player seeing stutter wants the second one, and the
  // gap between them is the headroom.
  ImGui::Text("Presented %.1f FPS    Throughput %.1f FPS    Frame %.2f ms", sActualFps, sThroughputFps,
              static_cast< double >(ImGui::GetIO().DeltaTime) * 1000.0);
  ImGui::SetItemTooltip("Presented is what reaches the screen. Throughput leaves out the wait for the\n"
                        "frame cap: the gap between the two is the headroom at the current frame cost.");
  if (SimAdaptive()) {
    ImGui::Text("Simulation %.1f ticks/s (adaptive)", sActualTps);
  } else {
    ImGui::Text("Simulation %.1f ticks/s (target %u)", sActualTps, SimRate());
  }

  bool smooth = sSmoothFrames;
  if (ImGui::Checkbox("Smooth uncapped frames", &smooth)) {
    PortDebug::SetSmoothFrames(smooth);
  }
  ItemHelp("With the FPS cap off, smooths mouse, gyro and twin-stick look and moving objects "
           "between the game's 60 Hz ticks. Aim and shots are unchanged.");

  {
    int catchUp = static_cast< int >(sCatchUpFrames);
    char format[48];
    std::snprintf(format, sizeof(format), "%%d frames (%.0f ms)", catchUp * 1000.0 / 60.0);
    if (ImGui::SliderInt("Catch-up", &catchUp, static_cast< int >(kCatchUpMin), static_cast< int >(kCatchUpMax),
                         format, ImGuiSliderFlags_AlwaysClamp)) {
      sCatchUpFrames = static_cast< unsigned >(catchUp);
      MarkDirty();
    }
    ItemHelp("When the game's ticks fall behind (a heavy room on a slow CPU), how much of a frame they "
             "may take to catch up, in 60 Hz frames' worth. Past it the game runs slow (slow motion) "
             "rather than drop further. More keeps the game nearer its full speed, at a lower frame "
             "rate. 2 is upstream's; the PS Vita starts at 4.");
  }

  ImGui::SeparatorText("Simulation rate (very experimental)");
  ImGui::TextColored(ThemeWarnColor(), "For testing only. The game was written for 60 Hz: other rates can\n"
                                       "break physics, enemy behaviour, cutscenes and scripted events.");
  bool adaptive = sSimAdaptive;
  if (ImGui::Checkbox("Adaptive (follow frame rate)", &adaptive)) {
    PortDebug::SetSimAdaptive(adaptive);
  }
  ItemHelp("Steps the game once per frame at the measured frame time, so a variable frame rate is "
           "matched. Leave the FPS cap off.");
  ImGui::BeginDisabled(adaptive);
  int simRate = static_cast< int >(sSimRate);
  if (ImGui::SliderInt("Sim Hz", &simRate, 30, 480)) {
    PortDebug::SetSimRate(static_cast< unsigned >(simRate));
  }
  ImGui::EndDisabled();
  ItemHelp("60 Hz is console-accurate and the only supported rate. Other rates change the game's "
           "logic speed; for smooth high frame rates use Smooth uncapped frames.");
  EndOriginalLocked(locked);
}

} // namespace PortDebug
