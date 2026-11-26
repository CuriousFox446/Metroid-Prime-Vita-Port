// F1 overlay: The Performance tab. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"

#include <aurora/imgui.h>
#include <dolphin/gx/GXExtra.h>
#include <imgui_internal.h>


namespace PortDebug {

namespace {
// The GPU cost test's button (PS Vita, 0.46.0 test 14): pressed, the test waits for the menu to close
// and some 30 frames more (GpuCostTestTick), so the menu is not among what the GPU draws.
bool sGpuTestArmed = false;
int sGpuTestClosedFrames = 0;
} // namespace

void GpuCostTestTick() {
  if (!sGpuTestArmed) {
    return;
  }
  if (Visible()) {
    sGpuTestClosedFrames = 0;
    return;
  }
  if (++sGpuTestClosedFrames >= 30) {
    sGpuTestArmed = false;
    sGpuTestClosedFrames = 0;
    GXPortRunGpuCostTest();
  }
}

void DrawPerformanceTab() {
  ImGui::SeparatorText("Frame rate");
  const bool locked = BeginOriginalLocked();
  bool frameLimit = sFrameLimitEnabled;
  if (ImGui::Checkbox("60 FPS cap (target)", &frameLimit)) {
    SetFrameLimitEnabled(frameLimit);
    if (frameLimit) {
      sFrameLimit30 = false;
    }
    MarkDirty();
  }
  // 0.46.0 test 68 (the user: a "30 FPS Cap (Target)" between the 60 FPS cap and the slider): the
  // other preset, one or the other. The game still steps at 60 Hz, two steps a frame.
  bool frameLimit30 = sFrameLimit30 && !sFrameLimitEnabled;
  if (ImGui::Checkbox("30 FPS cap (target)", &frameLimit30)) {
    sFrameLimit30 = frameLimit30;
    if (frameLimit30) {
      SetFrameLimitEnabled(false);
    }
    ResetDynamicRes();
    MarkDirty();
  }
  ItemHelp("Paces the game to 30 frames a second, each frame up two of the screen's refreshes, so "
           "they come evenly where 60 is out of reach. The game itself still runs at 60 Hz.");
  {
    ImGui::BeginDisabled(sFrameLimitEnabled || sFrameLimit30);
#ifdef __vita__
    // 30 to the screen's 60, where the top is no cap (saved as 0): the screen's own pace.
    int cap = sFpsCap > 0 ? sFpsCap : kFpsCapMax;
    if (ImGui::SliderInt("FPS cap", &cap, kFpsCapMin, kFpsCapMax, "%d FPS", ImGuiSliderFlags_AlwaysClamp)) {
      sFpsCap = ClampFpsCap(cap);
      ResetDynamicRes();
      MarkDirty();
    }
    ImGui::EndDisabled();
    ItemHelp("The most frames drawn a second with both caps above off, from 30 to the screen's 60. At "
             "30 each frame stays up two refreshes, so they come evenly.");
#else
    // One past the top is "Unlimited" (saved as 0).
    int cap = sFpsCap > 0 ? sFpsCap : kFpsCapMax + 1;
    if (ImGui::SliderInt("FPS cap", &cap, kFpsCapMin, kFpsCapMax + 1, cap > kFpsCapMax ? "Unlimited" : "%d FPS",
                         ImGuiSliderFlags_AlwaysClamp)) {
      sFpsCap = cap > kFpsCapMax ? 0 : ClampFpsCap(cap);
      ResetDynamicRes();
      MarkDirty();
    }
    ImGui::EndDisabled();
    ItemHelp("The most frames drawn a second with both caps above off. Vsync still limits it to the "
             "display's refresh rate.");
#endif
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
#if defined(__vita__)
  if (ImGui::Button(sGpuTestArmed ? "Close the menu to start##gpu-test" : "Run the GPU cost test##gpu-test")) {
    sGpuTestArmed = true;
  }
  ItemHelp("Stand still where the frame rate is low, press this and close the menu. About a second "
           "later, for some 20 seconds, parts of the picture are left out one kind at a time, and the "
           "log gets what each kind costs the GPU.");
#endif

  bool smooth = sSmoothFrames;
  if (ImGui::Checkbox("Smooth uncapped frames", &smooth)) {
    PortDebug::SetSmoothFrames(smooth);
  }
  ItemHelp("With the FPS cap off, smooths mouse, gyro and twin-stick look and moving objects "
           "between the game's 60 Hz ticks. Aim and shots are unchanged.");


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
  if (ImGui::SliderInt("Sim Hz", &simRate, static_cast< int >(kSimRateMin), static_cast< int >(kSimRateMax), "%d",
                       ImGuiSliderFlags_AlwaysClamp)) {
    PortDebug::SetSimRate(static_cast< unsigned >(simRate));
  }
  ImGui::EndDisabled();
  ItemHelp("60 Hz is console-accurate and the only supported rate. Other rates change the game's "
           "logic speed; for smooth high frame rates use Smooth uncapped frames.");
  EndOriginalLocked(locked);
}

} // namespace PortDebug
