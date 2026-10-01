// Minimal SceAudioOut declarations for PS5 payloads.
//
// The ps5-payload-sdk provides import shims (target/lib/libSceAudioOut.so,
// resolved by the payload loader against the system module at runtime) but
// ships no header for it. These signatures match the SDK's generated stubs
// and the PS4 headers the audio backend was written against; sceUserService
// supplies the foreground user id the controller-speaker port needs.
//
// Port/user rules measured on 11.60 (Radio app audio-probe.txt): MAIN opens
// only with the SYSTEM user id (0xFF); PADSPK opens only with the real
// foreground user id and only in S16 MONO (stereo -> 0x80260007).
#ifndef PS5_SCEAUDIO_H_
#define PS5_SCEAUDIO_H_

#include <stdint.h>

typedef int32_t SceUserServiceUserId;

#define SCE_USER_SERVICE_USER_ID_SYSTEM 0xFF

int32_t sceAudioOutInit(void);
int32_t sceAudioOutOpen(SceUserServiceUserId user_id, int port_type, int index,
                        uint32_t length, uint32_t sample_rate, uint32_t param_type);
int32_t sceAudioOutOutput(int32_t handle, const void *ptr);
int32_t sceAudioOutClose(int32_t handle);

// Submits one buffer per port and blocks once, keeping several ports in step.
typedef struct {
  int32_t handle;
  const void *pointer; // OpenOrbis' spelling, so the backend is shared with PS4
} SceAudioOutOutputParam;
int32_t sceAudioOutOutputs(SceAudioOutOutputParam *param, uint32_t num);

int32_t sceUserServiceInitialize(void *params);
int32_t sceUserServiceGetForegroundUser(SceUserServiceUserId *user_id);

// Names the PS4 audio backend uses (kept so the backend ports verbatim).
#define ORBIS_USER_SERVICE_USER_ID_SYSTEM SCE_USER_SERVICE_USER_ID_SYSTEM
#define ORBIS_AUDIO_OUT_PORT_TYPE_MAIN 0
#define ORBIS_AUDIO_OUT_PORT_TYPE_PADSPK 4
#define ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_MONO 0
#define ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO 1

#endif
