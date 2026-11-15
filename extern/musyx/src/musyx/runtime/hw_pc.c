#include "musyx/platform.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "musyx/adsr.h"
#include "musyx/assert.h"
#include "musyx/hardware.h"
#include "musyx/musyx.h"
#include "musyx/sal.h"
#include "musyx/synth.h"
#include "musyx/port_voices.h"
#include "musyx/pc_audio_math.h"

// Audio parameters
#define SAL_SAMPLES_PER_FRAME 160
#define SAL_SUBFRAMES 5
#define SAL_SAMPLES_PER_SUBFRAME (SAL_SAMPLES_PER_FRAME / SAL_SUBFRAMES)
#define SAL_STEREO_SAMPLES (SAL_SAMPLES_PER_FRAME * 2)
#define SAL_BUFFER_BYTES (SAL_STEREO_SAMPLES * sizeof(s16))
#define SRC_STAGING_SIZE 256
#define POLYPHASE_PHASES 128
#define POLYPHASE_TAPS 4
#define SINC_PHASES 512
#define SINC_TAPS 8
#define SINC_HALF_TAPS (SINC_TAPS / 2)

// Mode 0: GC polyphase, mode 1: windowed sinc
#ifndef SAL_RESAMPLE_MODE
#define SAL_RESAMPLE_MODE 0
#endif

#if SAL_RESAMPLE_MODE != 0 && SAL_RESAMPLE_MODE != 1
#error "SAL_RESAMPLE_MODE must be 0 (GC polyphase) or 1 (windowed sinc)"
#endif

// Double buffer for output
static s16 salOutputBuffers[2][SAL_STEREO_SAMPLES];
static u8 salOutputIndex = 0;

// SDL3 audio
static SDL_AudioStream* salAudioStream = NULL;
static SDL_Thread* salAudioThread = NULL;
static SDL_AtomicInt salAudioThreadRunning;
static SDL_Mutex* globalMutex;
static bool salAudioSubsystemInitialized;
static bool salTraceAudio;
static u64 salStatsStart, salStatsFrames, salStatsClipped;
static int salStatsPeak;

// Runtime mute (used by the port's debug overlay). The mixer still runs so the
// engine's timing is unchanged; only the pushed output is silenced.
static SDL_AtomicInt salMuted;
void salSetMuted(int muted) { SDL_SetAtomicInt(&salMuted, muted != 0); }

// The port's volume boost (F1 > Game > Audio above 100 percent, `music_boost`/`sfx_boost`): a
// linear factor on the decoded voice, 1.0 for the game's own maximum. The mix thread reads it once
// per voice, so a change lands between voices; a plain float is enough for that.
static float salVolumeBoostMusic = 1.0f;
static float salVolumeBoostSfx = 1.0f;

static SND_SOME_CALLBACK userCallback = NULL;

extern float gPortListenerHeading[3];

// Sample ids the user has muted from the debug overlay (seeded from
// MP_MUTE_SMP=65535,93). Muting silences the voice but keeps it running so its
// lifetime and timing are unchanged.
#define SAL_MAX_MUTED_SAMPLES 64
static u16 sMutedSamples[SAL_MAX_MUTED_SAMPLES];
static int sMutedSampleCount = 0;
static int sMuteListInitialized = 0;

static void salInitMuteList(void) {
  if (sMuteListInitialized) {
    return;
  }
  sMuteListInitialized = 1;
  const char* env = getenv("MP_MUTE_SMP");
  if (env == NULL) {
    return;
  }
  char buf[256];
  strncpy(buf, env, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  char* tok = strtok(buf, ",");
  while (tok != NULL && sMutedSampleCount < SAL_MAX_MUTED_SAMPLES) {
    sMutedSamples[sMutedSampleCount++] = (u16)strtoul(tok, NULL, 0);
    tok = strtok(NULL, ",");
  }
}

int MusyxPortIsSampleMuted(unsigned smpId) {
  salInitMuteList();
  for (int i = 0; i < sMutedSampleCount; ++i) {
    if (sMutedSamples[i] == (u16)smpId) {
      return 1;
    }
  }
  return 0;
}

void MusyxPortSetSampleMuted(unsigned smpId, int muted) {
  salInitMuteList();
  const u16 id = (u16)smpId;
  int idx = -1;
  for (int i = 0; i < sMutedSampleCount; ++i) {
    if (sMutedSamples[i] == id) {
      idx = i;
      break;
    }
  }
  if (muted && idx < 0) {
    if (sMutedSampleCount < SAL_MAX_MUTED_SAMPLES) {
      sMutedSamples[sMutedSampleCount++] = id;
    }
  } else if (!muted && idx >= 0) {
    sMutedSamples[idx] = sMutedSamples[--sMutedSampleCount];
  }
}

// Also drops MP_MUTE_SMP's ids for good: marking the list initialised keeps a later
// lookup from reading them back in.
void MusyxPortClearSampleMutes(void) {
  sMuteListInitialized = 1;
  sMutedSampleCount = 0;
}

// The port's two volume boosts: sequenced music voices and the sound effects voices. 1.0 is no
// boost, so the default mix stays exactly the game's.
void MusyxPortSetVolumeBoost(float music, float sfx) {
  salVolumeBoostMusic = music > 0.0f ? music : 1.0f;
  salVolumeBoostSfx = sfx > 0.0f ? sfx : 1.0f;
}

int MusyxPortGetMutedSamples(unsigned* out, int maxIds) {
  salInitMuteList();
  const int count = sMutedSampleCount < maxIds ? sMutedSampleCount : maxIds;
  for (int i = 0; i < count; ++i) {
    out[i] = sMutedSamples[i];
  }
  return count;
}

// ADPCM decode state per voice
static s16 adpcmYn1[SYNTH_MAX_VOICES];
static s16 adpcmYn2[SYNTH_MAX_VOICES];
static u8 adpcmPS[SYNTH_MAX_VOICES];
static u8 adpcmRestart[SYNTH_MAX_VOICES];

typedef struct VoiceResamplerState {
  s32 srcBuf[SRC_STAGING_SIZE];
  u32 srcCount;
  u32 srcConsumed;
  u32 curPos;
  u32 srcPosHi;
  s16 lastSamples[POLYPHASE_TAPS];
  u8 ended;
} VoiceResamplerState;

static VoiceResamplerState voiceResampler[SYNTH_MAX_VOICES];
static u32 voiceEffPitch[SYNTH_MAX_VOICES];
static s32 voiceRms[SYNTH_MAX_VOICES];
static s16 polyphaseTable[POLYPHASE_PHASES][POLYPHASE_TAPS];
static s16 sincTable[SINC_PHASES][SINC_TAPS];
static u8 resampleTablesInitialized = 0;

// Mix accumulation buffers
static s32 mixBufferL[SAL_SAMPLES_PER_FRAME];
static s32 mixBufferR[SAL_SAMPLES_PER_FRAME];
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
static s32 filterState[SYNTH_MAX_VOICES];
#endif

#define SURROUND_DOWNMIX_GAIN 23170

/* The mix of a frame in three parts (0.42.0 test 22). The whole mix (salCtrlDsp) ran under MusyX's
 * lock, which every sound call of the game's takes too (hwDisableIrq): in a fight, with the most
 * voices to mix and the most emitters to update, the game thread waited for the mix at its emitter
 * updates and pitch bends (test 21: busy 61% of the time while it held the frame rate back). Now:
 *  - the control pass (salCtrlDspControl), under the lock as before, settles what the game and the
 *    synth change (new voices, breaks, key offs, pitch, each subframe's ADSR, the volume ramps) in
 *    the whole mix's order, and copies what the render reads;
 *  - the render (salCtrlDspRender) decodes, resamples and mixes the voices with the lock open, as
 *    the GameCube's DSP mixed the parameter blocks the CPU had built while the game went on;
 *  - the finish (salCtrlDspFinish), under the lock again, writes the play positions back, retires
 *    the voices whose sample ran out, and runs the studio inputs, the aux effects (whose settings
 *    the game changes under the lock) and the downmix.
 * The render reads samples from aurora's ARAM buffer and writes the studio buffers salInitDspCtrl
 * made, both kept for the run; what it reads of a voice is copied. The output is the whole mix's.
 * MP_AUDIO_LOCKED_MIX=1 keeps the lock through the render, as before (to compare). */
#pragma pack(push, 4)
typedef struct SalVoiceRender {
  DSPvoice* vp;
  SAMPLE_INFO smp;              // the sample as of the control pass
  s16 coefTab[8][2];            // its ADPCM coefficients (the sample directory is the game's)
  u32 pitch[SAL_SUBFRAMES];     // each subframe's pitch after its control step (0: not played)
  u32 posHi;                    // the play position and address, for the finish to write back
  u32 posLo;
  u32 currentAddr;
  u16 adsrStart[SAL_SUBFRAMES];
  s16 adsrDelta[SAL_SUBFRAMES];
  s16 startVol[9];              // L R S, aux A L R S, aux B L R S as the frame starts
  s16 dVol[9];
  s16 loopY0;                   // the loop's ADPCM history and predictor (resetVoiceLoopState)
  s16 loopY1;
  u16 filterCoefA;
  u16 filterCoefB;
  u8 voiceIdx;
  u8 studio;                    // salStudioRenders index
  u8 firstSub;                  // the subframes played: firstSub up to, not including, endSub
  u8 endSub;
  u8 hasExtra;                  // an ADPCM sample whose info (coefficients, loop state) is copied
  u8 loopPS;
  u8 filterOn;
  u8 muted;
  u8 music;                     // the volume boost: music's (a sequencer voice) or the effects'
  u8 deactivated;               // the control pass retired the voice (ADSR ended, setup failed)
  u8 ended;                     // the render reached the end of the sample
} SalVoiceRender;

typedef struct SalStudioRender {
  DSPstudioinfo* stp;
  s32* main;                    // this frame's buffers: main[salFrame], auxA/B[salAuxFrame]
  s32* auxA;
  s32* auxB;
} SalStudioRender;
#pragma pack(pop)

static SalVoiceRender salVoiceRenders[SYNTH_MAX_VOICES];
static int salNumVoiceRenders;
static SalStudioRender salStudioRenders[8];
static int salNumStudioRenders;
static u64 salLastRenderNs;

/* For the port's [audio] line (MusyxPortTakeMixStats): the mixer's use of the lock and the other
 * threads' waits for it. Written with the lock held. */
static u32 salStatFrames, salStatVoices, salStatMaxVoices, salStatMixWaits;
static u32 salStatApiTakes, salStatApiWaits;
static u64 salStatLockedNs, salStatRenderNs, salStatMixWaitNs, salStatApiWaitNs, salStatApiMaxWaitNs;
static u64 salMixEnteredNs;
/* The mixer's thread (the audio device's, or salAudioThreadFunc), seen on its first frame. */
static SDL_ThreadID salMixThreadId;
static int salMixThreadSeen;
static int salMixCore = -1;
static int salUnlockedRender = -1;

static inline s16 clamp16(s32 v) {
  if (v > 32767)
    return 32767;
  if (v < -32768)
    return -32768;
  return (s16)v;
}

static inline s16 clampCoeff(s32 v) {
  if (v > 32767)
    return 32767;
  if (v < -32768)
    return -32768;
  return (s16)v;
}

static inline double sincUnit(double x) {
  if (SDL_fabs(x) < 1e-12)
    return 1.0;

  const double pix = x * SDL_PI_D;
  return SDL_sin(pix) / pix;
}

static void initResampleTables(void) {
  if (resampleTablesInitialized)
    return;

  double rawPolyphase[POLYPHASE_PHASES * POLYPHASE_TAPS];
  double maxPolyphaseSum = 0.0;

  for (u32 i = 0; i < SDL_arraysize(rawPolyphase); ++i) {
    const double phase = (double)i / (double)(SDL_arraysize(rawPolyphase) - 1);
    const double x = -2.0 + 4.0 * phase;
    const double window =
        0.54 - 0.46 * SDL_cos((2.0 * SDL_PI_D * i) / (SDL_arraysize(rawPolyphase) - 1));
    rawPolyphase[i] = sincUnit(x * 0.5) * window;
  }

  for (u32 phase = 0; phase < POLYPHASE_PHASES; ++phase) {
    const double sum = rawPolyphase[POLYPHASE_PHASES - 1 - phase] +
                       rawPolyphase[POLYPHASE_PHASES * 2 - 1 - phase] +
                       rawPolyphase[POLYPHASE_PHASES * 3 - 1 - phase] +
                       rawPolyphase[POLYPHASE_PHASES * 4 - 1 - phase];
    if (sum > maxPolyphaseSum)
      maxPolyphaseSum = sum;
  }

  if (maxPolyphaseSum <= 0.0)
    maxPolyphaseSum = 1.0;

  for (u32 phase = 0; phase < POLYPHASE_PHASES; ++phase) {
    polyphaseTable[phase][0] = clampCoeff(
        (s32)SDL_lround((rawPolyphase[POLYPHASE_PHASES - 1 - phase] / maxPolyphaseSum) * 32767.0));
    polyphaseTable[phase][1] = clampCoeff((s32)SDL_lround(
        (rawPolyphase[POLYPHASE_PHASES * 2 - 1 - phase] / maxPolyphaseSum) * 32767.0));
    polyphaseTable[phase][2] = clampCoeff((s32)SDL_lround(
        (rawPolyphase[POLYPHASE_PHASES * 3 - 1 - phase] / maxPolyphaseSum) * 32767.0));
    polyphaseTable[phase][3] = clampCoeff((s32)SDL_lround(
        (rawPolyphase[POLYPHASE_PHASES * 4 - 1 - phase] / maxPolyphaseSum) * 32767.0));
  }

  for (u32 phase = 0; phase < SINC_PHASES; ++phase) {
    const double frac = (double)phase / (double)SINC_PHASES;
    double taps[SINC_TAPS];
    double sum = 0.0;

    for (u32 tap = 0; tap < SINC_TAPS; ++tap) {
      const double x = (double)((s32)tap - (SINC_HALF_TAPS - 1)) - frac;
      double value = 0.0;

      if (SDL_fabs(x) < (double)SINC_HALF_TAPS)
        value = sincUnit(x) * sincUnit(x / (double)SINC_HALF_TAPS);

      taps[tap] = value;
      sum += value;
    }

    if (SDL_fabs(sum) < 1e-12) {
      memset(sincTable[phase], 0, sizeof(sincTable[phase]));
      sincTable[phase][SINC_HALF_TAPS - 1] = 0x7FFF;
      continue;
    }

    const double scale = 32767.0 / sum;
    s32 accum = 0;
    for (u32 tap = 0; tap < SINC_TAPS - 1; ++tap) {
      const s16 q = clampCoeff((s32)SDL_lround(taps[tap] * scale));
      sincTable[phase][tap] = q;
      accum += q;
    }

    sincTable[phase][SINC_TAPS - 1] = clampCoeff(32767 - accum);
  }

  resampleTablesInitialized = 1;
}

static inline u16 applyDeltaToLastVol(u16 lastVol, s16 deltaVol) {
  return (u16)((s32)(s16)lastVol + (s32)deltaVol * SAL_SAMPLES_PER_FRAME);
}

static inline void mixRampChannel(s32* dest, const s32* src, int nSamples, s16 startVol,
                                  s16 deltaVol, s16 envStart, s16 envDelta) {
  if (dest == NULL || (startVol == 0 && deltaVol == 0))
    return;

  for (int i = 0; i < nSamples; ++i) {
    s32 env = CLAMP((s32)envStart + (s32)envDelta * i, 0, 0x7FFF);
    s32 vol = startVol + deltaVol * i;
    s32 mixVol = musyxPcScaleQ15(vol, env);
    dest[i] = musyxPcClamp32((int64_t)dest[i] + musyxPcScaleQ15(src[i], mixVol));
  }
}

static inline void addThreeChannelBuffer(s32* dst, const s32* src, u16 vol) {
  if (dst == NULL || src == NULL || vol == 0)
    return;

  for (int i = 0; i < SAL_SAMPLES_PER_FRAME * 3; ++i)
    dst[i] = musyxPcClamp32((int64_t)dst[i] + musyxPcScaleQ15(src[i], vol));
}

static void downmixStudioToStereo(const s32* left, const s32* right, const s32* surround) {
  for (int i = 0; i < SAL_SAMPLES_PER_FRAME; ++i) {
    mixBufferL[i] = musyxPcClamp32((int64_t)mixBufferL[i] + left[i]);
    mixBufferR[i] = musyxPcClamp32((int64_t)mixBufferR[i] + right[i]);
    if (surround != NULL) {
      s32 surroundMix = musyxPcScaleQ15(surround[i], SURROUND_DOWNMIX_GAIN);
      mixBufferL[i] = musyxPcClamp32((int64_t)mixBufferL[i] + surroundMix);
      mixBufferR[i] = musyxPcClamp32((int64_t)mixBufferR[i] + surroundMix);
    }
  }
}

static void foldStereoToOutput(const s32* left, const s32* right) {
  for (int i = 0; i < SAL_SAMPLES_PER_FRAME; ++i) {
    mixBufferL[i] = musyxPcClamp32((int64_t)mixBufferL[i] + left[i]);
    mixBufferR[i] = musyxPcClamp32((int64_t)mixBufferR[i] + right[i]);
  }
}

static const s16 zeroCoefTab[8][2] = {{0}};

static inline int isVoiceADPCM(u8 compType) {
  return compType == 0 || compType == 1 || compType == 4 || compType == 5;
}

static const s16 (*getVoiceCoefTab(const SalVoiceRender* r))[2] {
  if (!isVoiceADPCM(r->smp.compType))
    return NULL;

  return r->hasExtra ? r->coefTab : zeroCoefTab;
}

static void resetVoiceLoopState(const SalVoiceRender* r) {
  const SAMPLE_INFO* smp = &r->smp;
  const u32 voiceIdx = r->voiceIdx;
  if (!isVoiceADPCM(smp->compType))
    return;

  if (musyxPcStreamedAdpcm(smp->compType)) {
    // A stream wraps a circular buffer of consecutive audio, not the song.
    // Match DSP loopType=1: retain predictor history and read the fresh header.
    adpcmRestart[voiceIdx] = 0;
    return;
  }

  if (r->hasExtra) {
    adpcmYn1[voiceIdx] = r->loopY1;
    adpcmYn2[voiceIdx] = r->loopY0;
    adpcmPS[voiceIdx] = r->loopPS;
  } else {
    adpcmYn1[voiceIdx] = 0;
    adpcmYn2[voiceIdx] = 0;
    adpcmPS[voiceIdx] = ((const u8*)smp->addr)[(smp->loop / 14) * 8];
  }
  adpcmRestart[voiceIdx] = 1;
}

/* The address hwGetPos reads back for a play position (a type without one keeps `current`). */
static u32 salCurrentAddr(const SAMPLE_INFO* smp, u32 srcPosHi, u32 current) {
  switch (smp->compType) {
  case 0:
  case 1:
  case 4:
  case 5:
    return (u32)((uintptr_t)smp->addr * 2 + (srcPosHi / 14) * 16 + 2 + (srcPosHi % 14));
  case 2:
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 2)
  case 6:
#endif
    /* Truncate before halving, as hwGetPos does ((u32)addr / 2); halving the
       64-bit pointer differs when bit 32 is set (high heaps, e.g. Android). */
    return (u32)(uintptr_t)smp->addr / 2 + srcPosHi;
  case 3:
    return (u32)((uintptr_t)smp->addr + srcPosHi);
  default:
    return current;
  }
}

static s32 sampleAtPos(SAMPLE_INFO* smp, u32 voiceIdx, u32 posHi, const s16 coefTab[8][2]) {
  switch (smp->compType) {
  case 0:
  case 1:
  case 4:
  case 5: {
    const u8* block = (const u8*)smp->addr + (posHi / 14) * 8;
    const unsigned sample = posHi % 14;
    if (!adpcmRestart[voiceIdx] && sample == 0) adpcmPS[voiceIdx] = block[0];
    adpcmRestart[voiceIdx] = 0;
    // Decode only the samples actually consumed. Decoding a whole block here
    // corrupts history when a loop starts or ends partway through that block.
    return musyxPcAdpcmSample(block, sample, adpcmPS[voiceIdx], coefTab,
                              &adpcmYn1[voiceIdx], &adpcmYn2[voiceIdx]);
  }
  case 2:
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 2)
  case 6:
#endif
  {
    /* PCM payloads remain in the GameCube's big-endian sample buffer. */
    const u8* sample = (const u8*)smp->addr + posHi * 2;
    return (s16)((u16)sample[0] << 8 | sample[1]);
  }
  case 3:
    return musyxPcPcm8(((u8*)smp->addr)[posHi]);
  default:
    return 0;
  }
}

/*
 * Decode sequential source-rate samples into a staging buffer.
 */
static int decodeSourceSamples(SalVoiceRender* r, s32* out, int maxSamples, int* hitEnd) {
  SAMPLE_INFO* smp = &r->smp;
  const u32 voiceIdx = r->voiceIdx;
  VoiceResamplerState* state = &voiceResampler[voiceIdx];
  const s16(*coefTab)[2] = getVoiceCoefTab(r);
  int count = 0;

  *hitEnd = 0;
  const u32 end = musyxPcLoopEnd(smp->length, smp->loop, smp->loopLength);

  for (; count < maxSamples; ++count) {
    if (state->srcPosHi >= end) {
      if (smp->loopLength > 0 && smp->loop < end && end - smp->loop <= smp->loopLength &&
          smp->loopLength <= end - smp->loop + 1) {
        state->srcPosHi = smp->loop + ((state->srcPosHi - end) % (end - smp->loop));
        resetVoiceLoopState(r);
      } else {
        *hitEnd = 1;
        state->ended = 1;
        break;
      }
    }

    out[count] = sampleAtPos(smp, voiceIdx, state->srcPosHi, coefTab);
    ++state->srcPosHi;
  }

  for (int i = count; i < maxSamples; ++i)
    out[i] = 0;

  return count;
}

static int fillSourceBuffer(SalVoiceRender* r, int outputSamples, u32 pitch) {
  VoiceResamplerState* state = &voiceResampler[r->voiceIdx];
  const u32 history = state->srcConsumed < SINC_HALF_TAPS ? state->srcConsumed : SINC_HALF_TAPS;
  const u32 available = state->srcCount - state->srcConsumed;
  u32 needed = (u32)(((u64)outputSamples * pitch + 0xFFFFu) >> 16);

  needed += SINC_HALF_TAPS + 1;
  if (available >= needed || state->ended)
    return state->ended;

  if (available + history > 0) {
    memmove(state->srcBuf, state->srcBuf + state->srcConsumed - history,
            (available + history) * sizeof(state->srcBuf[0]));
  }

  state->srcCount = available + history;
  state->srcConsumed = history;

  u32 targetCount = history + needed;
  if (targetCount > SRC_STAGING_SIZE)
    targetCount = SRC_STAGING_SIZE;

  if (targetCount > state->srcCount) {
    int hitEnd = 0;
    state->srcCount += (u32)decodeSourceSamples(r, state->srcBuf + state->srcCount,
                                                (int)(targetCount - state->srcCount), &hitEnd);
  }

  r->currentAddr = salCurrentAddr(&r->smp, state->srcPosHi, r->currentAddr);
  return state->ended;
}

static inline s32 readSourceBufferSample(const VoiceResamplerState* state, s32 idx) {
  if (idx < 0 || (u32)idx >= state->srcCount)
    return 0;

  return state->srcBuf[idx];
}

static int resampleVoice(u32 voiceIdx, s32* out, int numSamples, u32 pitch) {
  VoiceResamplerState* state = &voiceResampler[voiceIdx];

#if SAL_RESAMPLE_MODE == 0
  s16 temp[POLYPHASE_TAPS];
  u32 idx = 0;

  temp[idx++ & 3] = state->lastSamples[0];
  temp[idx++ & 3] = state->lastSamples[1];
  temp[idx++ & 3] = state->lastSamples[2];
  temp[idx++ & 3] = state->lastSamples[3];

  for (int i = 0; i < numSamples; ++i) {
    state->curPos += pitch;
    while (state->curPos >= 0x10000) {
      s32 sample = 0;
      if (state->srcConsumed < state->srcCount)
        sample = state->srcBuf[state->srcConsumed++];

      temp[idx++ & 3] = clamp16(sample);
      state->curPos -= 0x10000;
    }

    const u32 phase = (state->curPos & 0xFFFF) >> 9;
    const s16* c = polyphaseTable[phase];
    const long long t0 = temp[idx++ & 3];
    const long long t1 = temp[idx++ & 3];
    const long long t2 = temp[idx++ & 3];
    const long long t3 = temp[idx++ & 3];
    const long long sample = (t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15;

    out[i] = clamp16((s32)sample);
  }

  state->lastSamples[3] = temp[--idx & 3];
  state->lastSamples[2] = temp[--idx & 3];
  state->lastSamples[1] = temp[--idx & 3];
  state->lastSamples[0] = temp[--idx & 3];
#else
  for (int i = 0; i < numSamples; ++i) {
    const u32 phase = (state->curPos & 0xFFFF) >> 7;
    const s16* c = sincTable[phase];
    long long sample = 0;

    for (u32 tap = 0; tap < SINC_TAPS; ++tap) {
      const s32 srcIdx = (s32)state->srcConsumed + (s32)tap - (SINC_HALF_TAPS - 1);
      sample += (long long)readSourceBufferSample(state, srcIdx) * c[tap];
    }

    out[i] = clamp16((s32)(sample >> 15));

    const u32 step = state->curPos + pitch;
    state->srcConsumed += step >> 16;
    state->curPos = step & 0xFFFF;
  }
#endif

  return numSamples;
}

/*
 * Software voice rendering for one subframe (SAL_SAMPLES_PER_SUBFRAME samples) of a voice the
 * control pass set up: decodes samples, then mixes them into the studio's main and AUX buffers with
 * the subframe's volume ramps. It reads the voice's copy and the mixer's own per-voice state only,
 * so it runs with MusyX's lock open (salCtrlDspRender).
 */
static s32 voiceDecodeBuf[SAL_SAMPLES_PER_FRAME];

// Returns 1 if the voice finished playing (end of non-looping sample), 0 otherwise.
static int salRenderVoiceSubframe(SalVoiceRender* r, const SalStudioRender* sr, int subframe) {
  if (r->smp.addr == NULL)
    return 0;

  const u32 pitch = r->pitch[subframe];
  if (pitch == 0)
    return 0;

  const u32 voiceIdx = r->voiceIdx;
  /* Decode samples into temp buffer */
  VoiceResamplerState* state = &voiceResampler[voiceIdx];
  fillSourceBuffer(r, SAL_SAMPLES_PER_SUBFRAME, pitch);
  int nSamples = resampleVoice(voiceIdx, voiceDecodeBuf, SAL_SAMPLES_PER_SUBFRAME, pitch);
  voiceEffPitch[voiceIdx] = pitch;
  {
    long long sum = 0;
    for (int i = 0; i < nSamples; ++i)
      sum += (long long)voiceDecodeBuf[i] * voiceDecodeBuf[i];
    voiceRms[voiceIdx] = nSamples > 0 ? (s32)sqrt((double)sum / nSamples) : 0;
  }
  if (r->muted) {
    memset(voiceDecodeBuf, 0, (size_t)nSamples * sizeof(s32));
  }
  r->posHi = state->srcPosHi;
  r->posLo = state->curPos;
  int voiceDone = state->ended && state->srcConsumed >= state->srcCount;

#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
  if (r->filterOn) {
    s32 yn = filterState[voiceIdx];
    for (int i = 0; i < nSamples; ++i) {
      yn = ((s32)r->filterCoefA * voiceDecodeBuf[i] + (s32)r->filterCoefB * yn) >> 15;
      voiceDecodeBuf[i] = yn;
    }
    filterState[voiceIdx] = yn;
  }
#endif

  /* The port's volume boost above the game's own maximum: scale this voice, music (a voice the
   * sequencer allocated, fxFlag clear: synth.c picks the music fader the same way) or a sound
   * effect, and saturate to the decode buffer's 16-bit range. The hardware voice is the synth
   * voice of the same index. No boost means no multiply at all, so the default output stays
   * bit-identical. */
  {
    const float boost = r->music ? salVolumeBoostMusic : salVolumeBoostSfx;
    if (boost != 1.0f) {
      for (int i = 0; i < nSamples; ++i) {
        const float scaled = (float)voiceDecodeBuf[i] * boost;
        voiceDecodeBuf[i] =
            scaled >= 32767.0f ? 32767 : scaled <= -32768.0f ? -32768 : (s32)scaled;
      }
    }
  }

  /* L R S, aux A L R S, aux B L R S, as startVol and dVol. */
  const int frameOffset = subframe * SAL_SAMPLES_PER_SUBFRAME;
  s32* const studioMain = sr->main;
  s32* const auxACur = sr->auxA;
  s32* const auxBCur = sr->auxB;
  s32* const dest[9] = {
      studioMain,
      studioMain ? studioMain + SAL_SAMPLES_PER_FRAME : NULL,
      studioMain ? studioMain + SAL_SAMPLES_PER_FRAME * 2 : NULL,
      auxACur,
      auxACur ? auxACur + SAL_SAMPLES_PER_FRAME : NULL,
      auxACur ? auxACur + SAL_SAMPLES_PER_FRAME * 2 : NULL,
      auxBCur,
      auxBCur ? auxBCur + SAL_SAMPLES_PER_FRAME : NULL,
      auxBCur ? auxBCur + SAL_SAMPLES_PER_FRAME * 2 : NULL,
  };
  for (int k = 0; k < 9; ++k) {
    mixRampChannel(dest[k] ? dest[k] + frameOffset : NULL, voiceDecodeBuf, nSamples,
                   (s16)(r->startVol[k] + r->dVol[k] * frameOffset), r->dVol[k],
                   (s16)r->adsrStart[subframe], r->adsrDelta[subframe]);
  }

  return voiceDone;
}

/* The mix's first part, under the lock: each studio's voices settled in the whole mix's order (the
 * break cleanup, a new voice's start, each subframe's control step and ADSR, the volume ramps),
 * and a copy of what the render reads. A voice whose ADSR ends, or whose ADSR setup fails, is
 * retired here, as the whole mix retired it at that subframe; one whose sample runs out is
 * retired by the finish, once the render has found where. */
void salCtrlDspControl(void) {
  u8 st;
  DSPstudioinfo* stp;

  salNumVoiceRenders = 0;
  salNumStudioRenders = 0;

  for (st = 0, stp = dspStudio; st < salMaxStudioNum; ++st, ++stp) {
    if (stp->state != 1)
      continue;

    const u8 studioRender = (u8)salNumStudioRenders++;
    SalStudioRender* sr = &salStudioRenders[studioRender];
    sr->stp = stp;
    sr->main = stp->main[salFrame];
    sr->auxA = stp->auxA[salAuxFrame];
    sr->auxB = stp->auxB[salAuxFrame];

    /* Per-frame break cleanup, mirroring the original's pre-pass in
     * salBuildCommandList (hw_dspctrl.c) before the per-subframe mix loop.
     *
     * Without it a break relied entirely on the ADSR release reaching its end:
     * changed[] is only cleared by hwInitSamplePlayback, so the break bit stayed
     * set on a running voice and every frame re-ran adsrStartRelease, resetting
     * adsr.cnt before adsrHandle could count it down. A looping voice (the charge
     * beam hum) then never reached salDeactivateVoice and looped forever with no
     * synth voice left to stop it. Deactivating here also stops a repeated break
     * bit from reviving the voice.
     *
     * Only RUNNING voices (state != 0 && state != 1) are handled here. A new
     * voice (state == 1) is left to the startupBreak rule in the init block
     * below, so the stale break bit salActivateVoice leaves on a reused slot
     * cannot kill the voice that just took it. Voices are unlinked by
     * salDeactivateVoice, so the next pointer is read first and a dead voice
     * skipped. */
    for (DSPvoice* bp = stp->voiceRoot; bp != NULL;) {
      DSPvoice* nextBp = bp->next; /* save in case voice is deactivated */
      if (bp->state != 0 && bp->state != 1 &&
          (bp->postBreak != 0 || (bp->changed[0] & 0x20) != 0)) {
        if (bp->virtualSampleID != (u32)-1) {
          salSynthSendMessage(bp, 3);
        }
        salDeactivateVoice(bp);
        bp->startupBreak = 0;
      }
      bp = nextBp;
    }

    /* Settle all voices in this studio */
    DSPvoice* vp = stp->voiceRoot;
    while (vp != NULL) {
      DSPvoice* nextVp = vp->next; /* save in case voice is deactivated */
      if (vp->state != 0) {
        u32 voiceIdx = (u32)(vp - dspVoice);
        u8 mixStart = 0;

        /* New voice initialization (mirrors salBuildCommandList state==1 path) */
        if (vp->state == 1) {
          /* As Dolphin's salBuildCommandList: a voice broken in the frame it started
           * (startupBreak) never plays. Without this, a break written to changed[0]
           * for a voice that starts at a later subframe was skipped by the loop below,
           * and a looping sound (the charge beam hum) played on with no synth voice
           * left to stop it. Otherwise a break bit in changed[0] was carried over by
           * hwInitSamplePlayback from the slot's previous voice, not meant for this one. */
          if (vp->startupBreak) {
            vp->startupBreak = 0;
            salDeactivateVoice(vp);
            vp = nextVp;
            continue;
          }
          vp->changed[0] &= ~0x20;

          if (adsrSetup(&vp->adsr) != 0) {
            salSynthSendMessage(vp, 0);
            salDeactivateVoice(vp);
            vp = nextVp;
            continue;
          }

          vp->lastVolL = vp->volL;
          vp->lastVolR = vp->volR;
          vp->lastVolS = vp->volS;
          vp->lastVolLa = vp->volLa;
          vp->lastVolRa = vp->volRa;
          vp->lastVolSa = vp->volSa;
          vp->lastVolLb = vp->volLb;
          vp->lastVolRb = vp->volRb;
          vp->lastVolSb = vp->volSb;

          /* Initialize playback position based on compression type */
          switch (vp->smp_info.compType) {
          case 0:
          case 4:
          case 5:
            vp->playInfo.posHi = 0;
            vp->playInfo.posLo = 0;
            break;
          case 1: {
            u32 offset = (vp->smp_info.offset + 0xD) / 14;
            vp->playInfo.posHi = offset * 0xE;
            vp->playInfo.posLo = 0;
          } break;
          case 2:
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 2)
          case 6:
#endif
          case 3:
            vp->playInfo.posHi = vp->smp_info.offset;
            vp->playInfo.posLo = 0;
            break;
          }

          /* Reset ADPCM decode state for this voice */
          adpcmYn1[voiceIdx] = 0;
          adpcmYn2[voiceIdx] = 0;
          adpcmRestart[voiceIdx] = 1;
          adpcmPS[voiceIdx] = isVoiceADPCM(vp->smp_info.compType) && vp->smp_info.extraData != NULL
                                 ? ((DSPADPCMplusInfo*)vp->smp_info.extraData)->initialPS : 0;
          if (isVoiceADPCM(vp->smp_info.compType) && vp->smp_info.extraData == NULL && vp->smp_info.addr != NULL)
            adpcmPS[voiceIdx] = ((const u8*)vp->smp_info.addr)[(vp->playInfo.posHi / 14) * 8];
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
          filterState[voiceIdx] = 0;
#endif

          mixStart = vp->singleOffset;

          if (vp->smp_info.compType == 1) {
            u32 offset = (vp->smp_info.offset + 0xD) / 14;
            DSPADPCMplusInfo* adpcmInfo = (DSPADPCMplusInfo*)vp->smp_info.extraData;
            if (adpcmInfo != NULL) {
              adpcmYn2[voiceIdx] = adpcmInfo->blk[offset].Y0;
              adpcmYn1[voiceIdx] = adpcmInfo->blk[offset].Y1;
              adpcmPS[voiceIdx] = adpcmInfo->blk[offset].PS;
            }
          }

          voiceResampler[voiceIdx].srcCount = 0;
          voiceResampler[voiceIdx].srcConsumed = 0;
          voiceResampler[voiceIdx].curPos = 0;
          voiceResampler[voiceIdx].srcPosHi = vp->playInfo.posHi;
          memset(voiceResampler[voiceIdx].lastSamples, 0,
                 sizeof(voiceResampler[voiceIdx].lastSamples));
          voiceResampler[voiceIdx].ended = 0;
          vp->currentAddr =
              salCurrentAddr(&vp->smp_info, voiceResampler[voiceIdx].srcPosHi, vp->currentAddr);

          vp->state = 2;
        }

        if (sr->main != NULL) {
          const s16 dVol[9] = {
              ((s16)vp->volL - (s16)vp->lastVolL) / SAL_SAMPLES_PER_FRAME,
              ((s16)vp->volR - (s16)vp->lastVolR) / SAL_SAMPLES_PER_FRAME,
              ((s16)vp->volS - (s16)vp->lastVolS) / SAL_SAMPLES_PER_FRAME,
              ((s16)vp->volLa - (s16)vp->lastVolLa) / SAL_SAMPLES_PER_FRAME,
              ((s16)vp->volRa - (s16)vp->lastVolRa) / SAL_SAMPLES_PER_FRAME,
              ((s16)vp->volSa - (s16)vp->lastVolSa) / SAL_SAMPLES_PER_FRAME,
              ((s16)vp->volLb - (s16)vp->lastVolLb) / SAL_SAMPLES_PER_FRAME,
              ((s16)vp->volRb - (s16)vp->lastVolRb) / SAL_SAMPLES_PER_FRAME,
              ((s16)vp->volSb - (s16)vp->lastVolSb) / SAL_SAMPLES_PER_FRAME,
          };
          SalVoiceRender* r = &salVoiceRenders[salNumVoiceRenders];
          r->vp = vp;
          r->smp = vp->smp_info;
          r->hasExtra = 0;
          if (isVoiceADPCM(r->smp.compType) && r->smp.extraData != NULL) {
            const DSPADPCMplusInfo* info = (const DSPADPCMplusInfo*)r->smp.extraData;
            memcpy(r->coefTab, info->coefTab, sizeof(r->coefTab));
            r->loopY0 = info->loopY0;
            r->loopY1 = info->loopY1;
            r->loopPS = info->loopPS;
            r->hasExtra = 1;
          }
          r->posHi = vp->playInfo.posHi;
          r->posLo = vp->playInfo.posLo;
          r->currentAddr = vp->currentAddr;
          r->startVol[0] = (s16)vp->lastVolL;
          r->startVol[1] = (s16)vp->lastVolR;
          r->startVol[2] = (s16)vp->lastVolS;
          r->startVol[3] = (s16)vp->lastVolLa;
          r->startVol[4] = (s16)vp->lastVolRa;
          r->startVol[5] = (s16)vp->lastVolSa;
          r->startVol[6] = (s16)vp->lastVolLb;
          r->startVol[7] = (s16)vp->lastVolRb;
          r->startVol[8] = (s16)vp->lastVolSb;
          for (int k = 0; k < 9; ++k)
            r->dVol[k] = dVol[k];
#if MUSY_VERSION >= MUSY_VERSION_CHECK(2, 0, 1)
          r->filterOn = vp->filter.on;
          r->filterCoefA = vp->filter.coefA;
          r->filterCoefB = vp->filter.coefB;
#else
          r->filterOn = 0;
          r->filterCoefA = 0;
          r->filterCoefB = 0;
#endif
          r->voiceIdx = (u8)voiceIdx;
          r->studio = studioRender;
          r->firstSub = mixStart;
          r->endSub = mixStart;
          r->muted = MusyxPortIsSampleMuted(vp->smp_id) != 0;
          r->music = synthVoice != NULL && voiceIdx < synthInfo.voiceNum &&
                     synthVoice[voiceIdx].fxFlag == 0;
          r->deactivated = 0;
          r->ended = 0;

          for (u8 subframe = mixStart; subframe < SAL_SUBFRAMES; ++subframe) {
            if ((vp->changed[subframe] & 0x20) != 0) {
              adsrStartRelease(&vp->adsr, 10);
              vp->postBreak = 1;
            } else if (vp->postBreak == 0) {
              if ((vp->changed[subframe] & 0x40) != 0)
                adsrRelease(&vp->adsr);
              if ((vp->changed[subframe] & 8) != 0)
                vp->playInfo.pitch = vp->pitch[subframe];
            }

            if ((vp->changed[subframe] & 0x10) != 0) {
              if (adsrSetup(&vp->adsr) != 0) {
                salSynthSendMessage(vp, 0);
                salDeactivateVoice(vp);
                r->deactivated = 1;
                break;
              }
            }

            u16 adsrStart = 0;
            u16 adsrDelta = 0;
            u32 adsrDone = adsrHandle(&vp->adsr, &adsrStart, &adsrDelta);
            /* The pitch the whole mix read as it rendered this subframe. */
            u32 pitch = vp->playInfo.pitch;
            if (pitch == 0)
              pitch = vp->pitch[0];
            r->pitch[subframe] = pitch;
            r->adsrStart[subframe] = adsrStart;
            r->adsrDelta[subframe] = (s16)adsrDelta;
            r->endSub = (u8)(subframe + 1);

            if (adsrDone) {
              /* Played to its end in this subframe, as the whole mix rendered it before it
               * retired the voice. */
              salSynthSendMessage(vp, 0);
              salDeactivateVoice(vp);
              r->deactivated = 1;
              break;
            }
          }

          vp->lastVolL = applyDeltaToLastVol(vp->lastVolL, dVol[0]);
          vp->lastVolR = applyDeltaToLastVol(vp->lastVolR, dVol[1]);
          vp->lastVolS = applyDeltaToLastVol(vp->lastVolS, dVol[2]);
          vp->lastVolLa = applyDeltaToLastVol(vp->lastVolLa, dVol[3]);
          vp->lastVolRa = applyDeltaToLastVol(vp->lastVolRa, dVol[4]);
          vp->lastVolSa = applyDeltaToLastVol(vp->lastVolSa, dVol[5]);
          vp->lastVolLb = applyDeltaToLastVol(vp->lastVolLb, dVol[6]);
          vp->lastVolRb = applyDeltaToLastVol(vp->lastVolRb, dVol[7]);
          vp->lastVolSb = applyDeltaToLastVol(vp->lastVolSb, dVol[8]);

          if (r->endSub > r->firstSub)
            ++salNumVoiceRenders;
        }
      }
      vp = nextVp;
    }
  }

  salStatVoices += (u32)salNumVoiceRenders;
  if ((u32)salNumVoiceRenders > salStatMaxVoices)
    salStatMaxVoices = (u32)salNumVoiceRenders;
  ++salStatFrames;
}

/* The mix's second part, with the lock open (salPortUnlockedRender): the voices' sound into this
 * frame's studio buffers, in the control pass's order. */
void salCtrlDspRender(void) {
  const Uint64 start = SDL_GetTicksNS();

  memset(mixBufferL, 0, sizeof(mixBufferL));
  memset(mixBufferR, 0, sizeof(mixBufferR));

  for (int s = 0; s < salNumStudioRenders; ++s) {
    const SalStudioRender* sr = &salStudioRenders[s];
    /* Clear the current frame buffers only. Previous-frame data feeds studio inputs. */
    if (sr->main)
      memset(sr->main, 0, SAL_SAMPLES_PER_FRAME * 3 * sizeof(s32));

    /* Clear AUX buffers for the current aux frame */
    if (sr->auxA)
      memset(sr->auxA, 0, SAL_SAMPLES_PER_FRAME * 3 * sizeof(s32));
    if (sr->auxB)
      memset(sr->auxB, 0, SAL_SAMPLES_PER_FRAME * 3 * sizeof(s32));
  }

  for (int i = 0; i < salNumVoiceRenders; ++i) {
    SalVoiceRender* r = &salVoiceRenders[i];
    const SalStudioRender* sr = &salStudioRenders[r->studio];
    for (u8 subframe = r->firstSub; subframe < r->endSub; ++subframe) {
      if (salRenderVoiceSubframe(r, sr, subframe)) {
        r->ended = 1;
        break;
      }
    }
  }

  salLastRenderNs = SDL_GetTicksNS() - start;
}

/* The mix's third part, under the lock again: what the render found for the voices (their play
 * positions; a sample that ran out retires its voice, as the whole mix retired it at that
 * subframe), then each studio's inputs, aux effects and downmix, and the output. */
void salCtrlDspFinish(s16* dest) {
  salStatRenderNs += salLastRenderNs;
  salLastRenderNs = 0;

  for (int i = 0; i < salNumVoiceRenders; ++i) {
    SalVoiceRender* r = &salVoiceRenders[i];
    DSPvoice* vp = r->vp;
    /* A voice the control pass retired, or one the game stopped or started again while the render
     * ran (state 0 or 1, or another sample), keeps nothing of this render's but its sound. */
    if (r->deactivated || vp->state != 2 || vp->smp_info.addr != r->smp.addr)
      continue;
    vp->playInfo.posHi = r->posHi;
    vp->playInfo.posLo = r->posLo;
    vp->currentAddr = r->currentAddr;
    if (r->ended) {
      salSynthSendMessage(vp, 0);
      salDeactivateVoice(vp);
    }
  }

  for (int s = 0; s < salNumStudioRenders; ++s) {
    DSPstudioinfo* stp = salStudioRenders[s].stp;

    if (stp->main[salFrame] != NULL) {
      s32* studioMain = stp->main[salFrame];
      s32* auxACur = stp->auxA[salAuxFrame];
      s32* auxBCur = stp->auxB[salAuxFrame];
      for (u8 inputIdx = 0; inputIdx < stp->numInputs; ++inputIdx) {
        DSPinput* input = &stp->in[inputIdx];
        DSPstudioinfo* srcStudio = &dspStudio[input->studio];
        s32* srcMain = srcStudio->main[salFrame ^ 1];
        if (srcMain == NULL)
          continue;

        addThreeChannelBuffer(studioMain, srcMain, input->vol);
        addThreeChannelBuffer(auxACur, srcMain, input->volA);
        addThreeChannelBuffer(auxBCur, srcMain, input->volB);
      }
    }

    {
      s32* auxAWork = stp->auxA[(salAuxFrame + 2) % 3];
      if (auxAWork != NULL && stp->auxAHandler != NULL) {
        SND_AUX_INFO info;
        info.data.bufferUpdate.left = auxAWork;
        info.data.bufferUpdate.right = auxAWork + SAL_SAMPLES_PER_FRAME;
        info.data.bufferUpdate.surround = auxAWork + SAL_SAMPLES_PER_FRAME * 2;
        stp->auxAHandler(SND_AUX_REASON_BUFFERUPDATE, &info, stp->auxAUser);
      }

      if (stp->type == SND_STUDIO_TYPE_STD) {
        s32* auxBWork = stp->auxB[(salAuxFrame + 2) % 3];
        if (auxBWork != NULL && stp->auxBHandler != NULL) {
          SND_AUX_INFO info;
          info.data.bufferUpdate.left = auxBWork;
          info.data.bufferUpdate.right = auxBWork + SAL_SAMPLES_PER_FRAME;
          info.data.bufferUpdate.surround = auxBWork + SAL_SAMPLES_PER_FRAME * 2;
          stp->auxBHandler(SND_AUX_REASON_BUFFERUPDATE, &info, stp->auxBUser);
        }
      }
    }

    /* Accumulate master studio into final mix */
    if (stp->isMaster) {
      s32* studioMain = stp->main[salFrame];
      if (studioMain != NULL) {
        s32* studioL = studioMain;
        s32* studioR = studioMain + SAL_SAMPLES_PER_FRAME;
        s32* studioS = studioMain + SAL_SAMPLES_PER_FRAME * 2;
        downmixStudioToStereo(studioL, studioR, stp->type == SND_STUDIO_TYPE_DPL2 ? NULL : studioS);
      }

      /* Add processed AUX A (reverb output) to mix */
      s32* auxProcessed = stp->auxA[(salAuxFrame + 2) % 3];
      if (auxProcessed && stp->auxAHandler) {
        s32* auxL = auxProcessed;
        s32* auxR = auxProcessed + SAL_SAMPLES_PER_FRAME;
        s32* auxS = auxProcessed + SAL_SAMPLES_PER_FRAME * 2;
        downmixStudioToStereo(auxL, auxR, auxS);
      }

      if (stp->type == SND_STUDIO_TYPE_DPL2) {
        s32* dpl2Rear = stp->auxB[salAuxFrame];
        if (dpl2Rear != NULL)
          foldStereoToOutput(dpl2Rear, dpl2Rear + SAL_SAMPLES_PER_FRAME);
      } else {
        /* Add processed AUX B to mix for standard studios only. */
        s32* auxBProcessed = stp->auxB[(salAuxFrame + 2) % 3];
        if (auxBProcessed && stp->auxBHandler) {
          s32* auxBL2 = auxBProcessed;
          s32* auxBR2 = auxBProcessed + SAL_SAMPLES_PER_FRAME;
          s32* auxBS2 = auxBProcessed + SAL_SAMPLES_PER_FRAME * 2;
          downmixStudioToStereo(auxBL2, auxBR2, auxBS2);
        }
      }
    }
  }

  {
    static int sVoiceTrace = -1;
    if (sVoiceTrace < 0)
      sVoiceTrace = getenv("MP_LOG_VOICES") != NULL;
    if (sVoiceTrace) {
      static u32 sVoiceTraceCount = 0;
      if (++sVoiceTraceCount % 60 == 0) {
        int active = 0;
        for (int v = 0; v < salNumVoices; ++v) {
          DSPvoice* vp = &dspVoice[v];
          if (vp->state == 0)
            continue;
          ++active;
          fprintf(stderr,
                  "[voice] v=%d st=%u smp=%u info=%08X addr=%p len=%u loop=%u loopLen=%u comp=%u "
                  "pitch=%u eff=%u rms=%d volL=%u volR=%u\n",
                  v, vp->state, vp->smp_id, vp->smp_info.info, vp->smp_info.addr,
                  vp->smp_info.length, vp->smp_info.loop, vp->smp_info.loopLength,
                  vp->smp_info.compType, vp->playInfo.pitch, voiceEffPitch[v], voiceRms[v],
                  vp->volL, vp->volR);
        }
        fprintf(stderr, "[voice] active=%d heading=(%.3f,%.3f,%.3f)\n", active,
                gPortListenerHeading[0], gPortListenerHeading[1], gPortListenerHeading[2]);
      }
    }
  }

  /* Write interleaved stereo s16 to dest */
  if (dest) {
    for (int i = 0; i < SAL_SAMPLES_PER_FRAME; i++) {
      dest[i * 2 + 0] = clamp16(mixBufferL[i]);
      dest[i * 2 + 1] = clamp16(mixBufferR[i]);
    }
  }
}

/* The whole mix in one go, for a caller holding the lock throughout. */
void salCtrlDsp(s16* dest) {
  salCtrlDspControl();
  salCtrlDspRender();
  salCtrlDspFinish(dest);
}

#ifdef __vita__
/* The port's say on where the mixer runs (platform/vita/vita_platform.cpp): the fourth core when
 * CapUnlocker opens it. Returns the core it keeps to, -1 for the scheduler's choice. */
int MusyxPortOnMixThread(void);
#endif

/* The mixer's thread, on its first frame: for the lock statistics, and the port's placement. */
static void salNoteMixThread(void) {
  if (salMixThreadSeen)
    return;
  salMixThreadId = SDL_GetCurrentThreadID();
#ifdef __vita__
  salMixCore = MusyxPortOnMixThread();
#endif
  salMixThreadSeen = 1;
}

// Render one 5 ms frame and, with a device, queue it.
static void salRenderFrame(SDL_AudioStream* stream, int queuedBytes) {
  if (userCallback) {
    userCallback();
  }

  s16* buf = salOutputBuffers[salOutputIndex];
  if (salTraceAudio) {
    ++salStatsFrames;
    for (int i = 0; i < SAL_STEREO_SAMPLES; ++i) {
      const int magnitude = abs((int)buf[i]);
      if (magnitude > salStatsPeak) salStatsPeak = magnitude;
      if (magnitude >= 32767) ++salStatsClipped;
    }
    const u64 now = SDL_GetTicksNS();
    if (now - salStatsStart >= 1000000000ull) {
      const double seconds = (double)(now - salStatsStart) / 1000000000.0;
      fprintf(stderr, "[musyx] rate=%.0f samples/s queued=%.1f ms peak=%d clipped=%.3f%%\n",
              salStatsFrames * SAL_SAMPLES_PER_FRAME / seconds,
              queuedBytes > 0 ? queuedBytes * 1000.0 / (32000 * 4) : 0.0,
              salStatsPeak, 100.0 * salStatsClipped / (salStatsFrames * SAL_STEREO_SAMPLES));
      salStatsStart = now;
      salStatsFrames = salStatsClipped = 0;
      salStatsPeak = 0;
    }
  }
  if (stream != NULL) {
    if (SDL_GetAtomicInt(&salMuted)) {
      memset(buf, 0, SAL_BUFFER_BYTES);
    }
    SDL_PutAudioStreamData(stream, buf, SAL_BUFFER_BYTES);
  }
}

// The device pulls: render only what it asks for, so a new sound waits one device period rather
// than a producer-side queue (that queue held up to 60 ms and made rapid fire audibly late).
static void SDLCALL salStreamCallback(void* userdata, SDL_AudioStream* stream, int additional,
                                      int total) {
  (void)userdata;
  (void)total;
  salNoteMixThread();
  for (int left = additional; left > 0; left -= SAL_BUFFER_BYTES) {
    salRenderFrame(stream, salTraceAudio ? SDL_GetAudioStreamQueued(stream) : 0);
  }
}

// Without a device, keep the synth running in real time.
static int salAudioThreadFunc(void* data) {
  (void)data;
  salNoteMixThread();
  while (SDL_GetAtomicInt(&salAudioThreadRunning)) {
    salRenderFrame(NULL, 0);
    SDL_Delay(5); // one 160-sample frame at 32 kHz
  }
  return 0;
}

bool salInitAi(SND_SOME_CALLBACK callback, u32 flags, u32* outFreq) {
  (void)flags;
  memset(salOutputBuffers, 0, sizeof(salOutputBuffers));
  salOutputIndex = 0;
  userCallback = callback;
  salTraceAudio = getenv("MP_AUDIO_STATS") != NULL;
  salStatsStart = SDL_GetTicksNS();
  salStatsFrames = salStatsClipped = 0;
  salStatsPeak = 0;

  memset(adpcmYn1, 0, sizeof(adpcmYn1));
  memset(adpcmYn2, 0, sizeof(adpcmYn2));
  memset(adpcmPS, 0, sizeof(adpcmPS));
  memset(adpcmRestart, 0, sizeof(adpcmRestart));
  memset(voiceResampler, 0, sizeof(voiceResampler));
  initResampleTables();

  salAudioSubsystemInitialized = SDL_InitSubSystem(SDL_INIT_AUDIO);

  SDL_AudioSpec spec = {
      .format = SDL_AUDIO_S16,
      .channels = 2,
      .freq = *outFreq,
  };
  if (salAudioSubsystemInitialized) {
    salAudioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                               salStreamCallback, NULL);
  }
  if (salAudioStream == NULL) {
    fprintf(stderr, "MusyX: audio device unavailable, running silently: %s\n", SDL_GetError());
  }

  synthInfo.numSamples = 0x20;
  *outFreq = spec.freq;
  return true;
}

bool salStartAi() {
  if (salAudioStream != NULL)
    return SDL_ResumeAudioStreamDevice(salAudioStream);

  SDL_SetAtomicInt(&salAudioThreadRunning, 1);
  salAudioThread = SDL_CreateThread(salAudioThreadFunc, "MusyX Audio", NULL);
  if (!salAudioThread) {
    SDL_SetAtomicInt(&salAudioThreadRunning, 0);
    return false;
  }
  return true;
}

bool salExitAi() {
  if (salAudioStream) {
    SDL_DestroyAudioStream(salAudioStream);
    salAudioStream = NULL;
  }
  if (salAudioThread) {
    SDL_SetAtomicInt(&salAudioThreadRunning, 0);
    SDL_WaitThread(salAudioThread, NULL);
    salAudioThread = NULL;
  }
  if (salAudioSubsystemInitialized) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    salAudioSubsystemInitialized = false;
  }
  return true;
}

void* salAiGetDest() {
  salOutputIndex ^= 1;
  return salOutputBuffers[salOutputIndex];
}

bool salInitDsp(u32 flags) {
  (void)flags;
  return true;
}

bool salExitDsp() { return true; }

void salStartDsp(u16* cmdList) {
  (void)cmdList;
  /* no-op */
}

void hwInitIrq() {
  globalMutex = SDL_CreateMutex();
  /* Start with IRQs disabled (locked), matching hwIrqLevel=1 on Dolphin.
   * hwEnableIrq() in hwInit() will unlock. */
  SDL_LockMutex(globalMutex);
}

void hwExitIrq() {
  if (globalMutex != NULL) {
    SDL_DestroyMutex(globalMutex);
    globalMutex = NULL;
  }
}

void hwEnableIrq() { SDL_UnlockMutex(globalMutex); }

/* The game's sound calls (and the synth's) take the lock here: counted, and timed when they found
 * it held, for the [audio] line. The counts are updated holding the lock. */
void hwDisableIrq() {
  if (SDL_TryLockMutex(globalMutex)) {
    ++salStatApiTakes;
    return;
  }
  const Uint64 start = SDL_GetTicksNS();
  SDL_LockMutex(globalMutex);
  const Uint64 waited = SDL_GetTicksNS() - start;
  ++salStatApiTakes;
  if (salMixThreadSeen && SDL_GetCurrentThreadID() == salMixThreadId) {
    ++salStatMixWaits;
    salStatMixWaitNs += waited;
  } else {
    ++salStatApiWaits;
    salStatApiWaitNs += waited;
    if (waited > salStatApiMaxWaitNs)
      salStatApiMaxWaitNs = waited;
  }
}

/* The mixer's own sections (snd_handle_irq): their time holding the lock, and its waits for it. */
void hwIRQEnterCritical() {
  if (!SDL_TryLockMutex(globalMutex)) {
    const Uint64 start = SDL_GetTicksNS();
    SDL_LockMutex(globalMutex);
    ++salStatMixWaits;
    salStatMixWaitNs += SDL_GetTicksNS() - start;
  }
  salMixEnteredNs = SDL_GetTicksNS();
}

void hwIRQLeaveCritical() {
  salStatLockedNs += SDL_GetTicksNS() - salMixEnteredNs;
  SDL_UnlockMutex(globalMutex);
}

/* Whether the render runs with the lock open (snd_handle_irq): yes unless MP_AUDIO_LOCKED_MIX=1. */
int salPortUnlockedRender(void) {
  if (salUnlockedRender < 0) {
    const char* env = getenv("MP_AUDIO_LOCKED_MIX");
    salUnlockedRender = !(env != NULL && env[0] == '1');
  }
  return salUnlockedRender;
}

void MusyxPortTakeMixStats(MusyxPortMixStats* out) {
  if (out == NULL)
    return;
  memset(out, 0, sizeof(*out));
  out->core = salMixCore;
  out->unlockedRender = salUnlockedRender < 0 ? 1 : salUnlockedRender;
  if (globalMutex == NULL)
    return;
  SDL_LockMutex(globalMutex);
  out->lockedNs = salStatLockedNs;
  out->renderNs = salStatRenderNs;
  out->mixWaitNs = salStatMixWaitNs;
  out->apiWaitNs = salStatApiWaitNs;
  out->apiMaxWaitNs = salStatApiMaxWaitNs;
  out->frames = salStatFrames;
  out->voices = salStatVoices;
  out->maxVoices = salStatMaxVoices;
  out->mixWaits = salStatMixWaits;
  out->apiTakes = salStatApiTakes;
  out->apiWaits = salStatApiWaits;
  salStatLockedNs = salStatRenderNs = salStatMixWaitNs = salStatApiWaitNs = salStatApiMaxWaitNs = 0;
  salStatFrames = salStatVoices = salStatMaxVoices = salStatMixWaits = 0;
  salStatApiTakes = salStatApiWaits = 0;
  SDL_UnlockMutex(globalMutex);
}

// Snapshot of the active voices for the debug overlay. Reads guest state from
// the UI thread; a torn read only affects a diagnostic display.
int MusyxPortCopyVoices(PortMusyxVoice* out, int maxVoices) {
  if (dspVoice == NULL || out == NULL || maxVoices <= 0) {
    return 0;
  }
  int count = 0;
  for (int v = 0; v < salNumVoices && count < maxVoices; ++v) {
    const DSPvoice* vp = &dspVoice[v];
    if (vp->state == 0) {
      continue;
    }
    out[count].smpId = vp->smp_id;
    out[count].compType = vp->smp_info.compType;
    out[count].looped = vp->smp_info.loopLength != 0;
    out[count].length = vp->smp_info.length;
    out[count].pitch = vp->playInfo.pitch != 0 ? vp->playInfo.pitch : vp->pitch[0];
    out[count].rms = voiceRms[v];
    out[count].volL = vp->volL;
    out[count].volR = vp->volR;
    ++count;
  }
  return count;
}
