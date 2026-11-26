#ifndef _PORTMENUZOOM
#define _PORTMENUZOOM

#ifdef TARGET_PC
#include "Kyoto/Graphics/CGraphics.hpp"
#include "port_debug.h"
#include "rstl/math.hpp"

#include <dolphin/gx/GXExtra.h>

// Port (0.46.0 test 22): the menus zoomed to fill a wide screen (PortDebug::ZoomMenus) for one
// CFrontEndUI::Draw: GX's viewport scaled about the screen's centre until the 4:3 frame's 640
// units span the viewport's width (the user: "zoom it in to make it fit the Vita's screen (not by
// stretching)"). The menus' picture is a band letterboxed inside that frame, the title and file
// select movies' and the frames over them, so the zoom cuts only black above and below it. It stops
// at 4/3, where a 16:9 band (three quarters of the frame's height) meets the screen's top and
// bottom, for screens wider still. The layout, and GXGetViewport, stay as they were. The frame is
// 640x480 units (the GameCube's 480 lines), so on the Vita (846x480) the zoom is about 1.32, and the
// screen shows the frame's lines 58 to 422: the title movie's 16:9 band (0.46.0 test 23, its frames
// from the user's disc). Since test 54 the credits too (CCredits::Draw: their movie is 4:3 with a
// 16:9 band, the text and bars laid out for it; the user: "Gonna need to do that for the credits
// too"). Shared from CFrontEndUI.cpp, where it began.
inline float PortMenuZoomFactor() {
  if (!PortDebug::ZoomMenus() || PortDebug::AspectMode() == PortDebug::kAspect_4_3) {
    return 1.f;
  }
  const CViewport& vp = CGraphics::GetViewport();
  if (vp.mWidth <= 0 || vp.mHeight <= 0) {
    return 1.f;
  }
  const float zoom = rstl::min_val(static_cast< float >(vp.mWidth) / 640.f, 4.f / 3.f);
  return zoom > 1.01f ? zoom : 1.f;
}

// The zoom for one scope (a Draw), about the viewport's centre; off again at its end. `on` false
// leaves that Draw unzoomed (0.46.0 test 69: the attract movies, at their 4:3).
class PortMenuZoom {
public:
  PortMenuZoom() : PortMenuZoom(true) {}
  explicit PortMenuZoom(bool on) {
    const float zoom = on ? PortMenuZoomFactor() : 1.f;
    if (zoom <= 1.f) {
      return;
    }
    const CViewport& vp = CGraphics::GetViewport();
    GXPortSetViewportZoom(zoom, static_cast< float >(vp.mLeft) + 0.5f * static_cast< float >(vp.mWidth),
                          static_cast< float >(vp.mTop) + 0.5f * static_cast< float >(vp.mHeight));
    mOn = true;
  }
  ~PortMenuZoom() {
    if (mOn) {
      GXPortSetViewportZoom(1.f, 0.f, 0.f);
    }
  }
  PortMenuZoom(const PortMenuZoom&) = delete;
  PortMenuZoom& operator=(const PortMenuZoom&) = delete;

private:
  bool mOn = false;
};
#endif

#endif // _PORTMENUZOOM
