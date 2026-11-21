// F1 overlay: The Performance tab. State and helpers shared with the other parts are in
// debug_ui_internal.h.

#include "debug_ui_internal.h"
#include "port_log.h"

#include <aurora/imgui.h>
#include <dolphin/gx/GXExtra.h>
#include <imgui_internal.h>


namespace PortDebug {

namespace {
// The GPU cost test's button (PS Vita, 0.46.0 test 14): pressed, the test waits for the menu to close
// and some 30 frames more (GpuCostTestTick), so the menu is not among what the GPU draws.
bool sGpuTestArmed = false;
int sGpuTestClosedFrames = 0;

// 0.46.0 test 41's diagnostics, for this session only: the kinds of drawing left out (EDiag, read by
// the game's code on its thread) and the Vita renderer's own (GX_PORT_DIAG_).
std::atomic<unsigned> sDiagOff{0};

#if defined(__vita__)
uint32_t sRendererDiag = 0;

struct DiagBox {
  const char* label;
  unsigned bit;
  bool renderer; // a GX_PORT_DIAG_ bit rather than an EDiag one
  const char* help;
};
constexpr DiagBox kDiagBoxes[] = {
    {"No mipmaps: compressed textures", GX_PORT_DIAG_NO_MIPS_BC1, true,
     "The compressed textures (most of the world's) drawn from their full-size level alone, as "
     "before they had smaller levels: far and slanted surfaces shimmer more."},
    {"No mipmaps: colour textures", GX_PORT_DIAG_NO_MIPS_RGBA, true,
     "The other colour textures (paletted ones among them) drawn from their full-size level alone."},
    {"No mipmaps: greyscale textures", GX_PORT_DIAG_NO_MIPS_INTENSITY, true,
     "The greyscale textures, with or without their own alpha, drawn from their full-size level alone."},
    {"Leave out floor reflections", kDiag_Reflections, false,
     "Shiny floors and metal reflect nothing: the reflection the game draws in a corner of the frame "
     "and copies is neither drawn nor copied."},
    {"Leave out screen-distorting effects", kDiag_Distortion, false,
     "Heat haze, Phazon shimmer and the other effects that bend what is behind them are not drawn, "
     "nor their copy of the screen made."},
    {"Leave out decals", kDiag_Decals, false, "Shot marks, scorches and the other marks left on walls and floors."},
    {"Leave out projected shadows", kDiag_ProjectedShadows, false,
     "The shadows some creatures cast on the floor from a texture of their outline."},
    {"Leave out blob shadows", kDiag_BlobShadows, false, "The round shadows under creatures and pickups."},
    {"Leave out fog volumes", kDiag_FogVolumes, false, "The patches of fog in a room, such as the mist over a pit."},
    {"Work out every draw's material again (slow)", GX_PORT_DIAG_MATERIALS, true,
     "Each draw takes its textures, blending and program from the game's state afresh, nothing kept "
     "from the draws before it. The frame rate drops."},
    {"Wait for the GPU after each frame (slow)", GX_PORT_DIAG_FINISH, true,
     "The renderer waits for the GPU to finish each frame before the next, so nothing a frame drew "
     "with is let go or written again while the GPU may still read it. The frame rate drops."},
};

void DrawDiagnostics() {
  ImGui::SeparatorText("Diagnostics (this session only)");
  ImGui::TextWrapped("Each box leaves something out, or has the renderer redo or wait for something, until it is "
                     "unticked. Nothing here is saved: all are off at the next start.");
  for (const DiagBox& box : kDiagBoxes) {
    bool on = box.renderer ? (sRendererDiag & box.bit) != 0 : (sDiagOff.load(std::memory_order_relaxed) & box.bit) != 0;
    if (ImGui::Checkbox(box.label, &on)) {
      if (box.renderer) {
        sRendererDiag = on ? sRendererDiag | box.bit : sRendererDiag & ~box.bit;
        GXPortSetVitaDiagnostics(sRendererDiag);
      } else if (on) {
        sDiagOff.fetch_or(box.bit, std::memory_order_relaxed);
      } else {
        sDiagOff.fetch_and(~box.bit, std::memory_order_relaxed);
      }
      PortLog::Write("port: diagnostic \"%s\" %s\n", box.label, on ? "on" : "off");
    }
    ItemHelp(box.help);
  }
}
#endif
} // namespace

bool DiagOff(EDiag part) { return (sDiagOff.load(std::memory_order_relaxed) & part) != 0; }

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
    MarkDirty();
  }
  {
    ImGui::BeginDisabled(sFrameLimitEnabled);
#ifdef __vita__
    // 30 to the screen's 60, where the top is no cap (saved as 0): the screen's own pace.
    int cap = sFpsCap > 0 ? sFpsCap : kFpsCapMax;
    if (ImGui::SliderInt("FPS cap", &cap, kFpsCapMin, kFpsCapMax, "%d FPS", ImGuiSliderFlags_AlwaysClamp)) {
      sFpsCap = ClampFpsCap(cap);
      ResetDynamicRes();
      MarkDirty();
    }
    ImGui::EndDisabled();
    ItemHelp("The most frames drawn a second with the 60 FPS cap off, from 30 to the screen's 60. At 30 "
             "each frame stays up two refreshes, so they come evenly.");
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
    ItemHelp("The most frames drawn a second with the 60 FPS cap off. Vsync still limits it to the "
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
#if defined(__vita__)
  DrawDiagnostics();
#endif
}

} // namespace PortDebug
