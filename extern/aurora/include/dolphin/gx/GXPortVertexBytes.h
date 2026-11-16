#ifndef DOLPHIN_GXPORTVERTEXBYTES_H
#define DOLPHIN_GXPORTVERTEXBYTES_H

#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Port (PS Vita, 0.46.0 test 8): between GXBegin and GXEnd, `size` bytes of vertices written as
// they are, in GX's byte order: what a run of GXPosition and GXTexCoord calls would have written, put
// together by the caller and sent in one write. Each of those calls is a function of its own that
// writes its components one at a time, every write checking the FIFO's room again: Phendrana's snow
// made ~55,000 such calls a frame, some 7 ms of the game thread's.
void GXPortWriteVertexBytes(const void* data, u32 size);

#ifdef __cplusplus
}
#endif

#endif
