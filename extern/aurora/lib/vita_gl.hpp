#pragma once

// The PS Vita's screen through vitaGL (a GL layer over GXM), in place of SDL's GXM renderer:
// Phase 2 of the Vita port (docs/VITA_PHASE2.md). For now it clears the screen and draws the
// ImGui overlay with ImGui's OpenGL 2 backend; the game's GX draws still go to the null WebGPU.
//
// vitaGL is not thread-safe: after initialize (game thread) every GL call is made on Aurora's
// render worker, where frames are presented.

#include <cstdint>

#include <imgui.h>

namespace aurora::vita_gl {
#ifdef AURORA_VITA_GL
// Starts vitaGL at the Vita's 960x544. False if it could not (then SDL's renderer is used).
bool initialize() noexcept;
bool active() noexcept;
// ImGui's renderer backend (game thread, after ImGui's context exists).
void init_imgui() noexcept;
void shutdown_imgui() noexcept;
// The font atlas was rebuilt: its texture is uploaded again before the next present.
void fonts_changed() noexcept;
// Render worker: clears the screen, draws the overlay (null: none) and swaps.
void present(ImDrawData* overlay) noexcept;
// An RGBA8 texture for ImGui. Game thread; waits for the render worker to make it.
ImTextureID add_texture(uint32_t width, uint32_t height, const uint8_t* rgba8) noexcept;
void remove_texture(ImTextureID id) noexcept;
#else
inline bool initialize() noexcept { return false; }
inline bool active() noexcept { return false; }
inline void init_imgui() noexcept {}
inline void shutdown_imgui() noexcept {}
inline void fonts_changed() noexcept {}
inline void present(ImDrawData*) noexcept {}
inline ImTextureID add_texture(uint32_t, uint32_t, const uint8_t*) noexcept { return ImTextureID{}; }
inline void remove_texture(ImTextureID) noexcept {}
#endif
} // namespace aurora::vita_gl
