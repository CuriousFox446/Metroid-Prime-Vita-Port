#ifndef MUSYX_PORT_VOICES_H
#define MUSYX_PORT_VOICES_H

#include <stdint.h>

// Port-only bridge so the debug overlay can list the live software voices and
// mute a sample by id. Implemented by hw_pc.c. This header must not pull in the
// MusyX headers: they leave `#pragma pack(1)` in effect, which breaks unrelated
// includes in the C++ overlay.
#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)
typedef struct PortMusyxVoice {
  uint16_t smpId;
  uint8_t compType;
  uint8_t looped;
  uint32_t length;
  uint32_t pitch;
  int32_t rms;
  uint16_t volL;
  uint16_t volR;
} PortMusyxVoice;
#pragma pack(pop)

// Copies the currently active voices (up to maxVoices) and returns the count.
int MusyxPortCopyVoices(PortMusyxVoice* out, int maxVoices);
int MusyxPortIsSampleMuted(unsigned smpId);
void MusyxPortSetSampleMuted(unsigned smpId, int muted);
void MusyxPortClearSampleMutes(void);
// The port's volume boost: a linear factor (1.0 = none) on the music voices (the sequencer's) and
// on the sound effects voices, applied to each voice's decoded samples in the mix.
void MusyxPortSetVolumeBoost(float music, float sfx);
// Copies the muted sample ids (up to maxIds) and returns the count.
int MusyxPortGetMutedSamples(unsigned* out, int maxIds);

// MusyX's lock since the last call (the port's [audio] line, 0.42.0 test 22): the mixer's time
// holding it and its voice renders, and the waits for it of the other threads (the game's sound
// calls). Packed explicitly: the MusyX headers' packing would differ between its users.
#pragma pack(push, 8)
typedef struct MusyxPortMixStats {
  uint64_t lockedNs;     // the mixer holding the lock (all its sections)
  uint64_t renderNs;     // its voice renders (with the lock open unless unlockedRender is 0)
  uint64_t mixWaitNs;    // the mixer waiting for the lock (a sound call holding it)
  uint64_t apiWaitNs;    // the other threads waiting for it
  uint64_t apiMaxWaitNs; // their longest wait
  uint32_t frames;       // 5 ms frames mixed
  uint32_t voices;       // voices rendered over those frames
  uint32_t maxVoices;    // the most in one frame
  uint32_t mixWaits;
  uint32_t apiTakes;     // sound calls that took the lock (hwDisableIrq)
  uint32_t apiWaits;     // of them, the ones that found it held
  int32_t core;          // the mixer's core (PS Vita), -1 for the scheduler's choice
  int32_t unlockedRender;
} MusyxPortMixStats;
#pragma pack(pop)
void MusyxPortTakeMixStats(MusyxPortMixStats* out);

#ifdef __cplusplus
}
#endif

#endif
