// Minimal SceAudioOut declarations for PS5 payloads.
//
// The ps5-payload-sdk provides import shims (target/lib/libSceAudioOut.so,
// resolved by the payload loader against the system module at runtime) but
// ships no header for it. These signatures match the SDK's generated stubs
// and the PS4 headers the audio backend was written against; sceUserService
// is linked only to satisfy the stub archive, its functions are not called
// (the PS4 port passes the system user id constant directly).
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

// Names the PS4 audio backend uses (kept so the backend ports verbatim).
#define ORBIS_USER_SERVICE_USER_ID_SYSTEM SCE_USER_SERVICE_USER_ID_SYSTEM
#define ORBIS_AUDIO_OUT_PORT_TYPE_MAIN 0
#define ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO 1

#endif
