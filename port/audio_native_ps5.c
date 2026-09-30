// PS5 native audio output, ported from audio_native_ps4.c (C7, proven on
// console). Raw sceAudioOut via the SDK's import shim (see ps5_sceaudio.h);
// the PS4 SDL audio backend crashes on init, so SDL_OpenAudioDevice was
// never an option there and isn't one here either.
//
// The USB engine (usbio_ps4.c) captures M8 PCM (44.1k s16 stereo) into a
// ring; THIS file owns a consumer thread that resamples to the console's
// 48 kHz and calls sceAudioOutOutput (blocking — it IS the pacing).

#include <string.h>

#include <SDL.h>
#include <SDL_thread.h>

#include "ps5_sceaudio.h"

#include "audio.h"
#include "ps4_shims.h"
#include "usb_ps4.h"
#include "usbio_ps4.h"

#define IN_RATE 44100
#define OUT_RATE 48000
#define OUT_GRANULARITY 256 // frames per sceAudioOutOutput call (~5.3 ms)
#define PREBUFFER_BYTES (8 * 1024)
#define HIGH_WATER_BYTES (64 * 1024) // ~370 ms: clock drift, trim back down

static volatile int audio_running = 0;
static SDL_Thread *out_thread = NULL;
static int32_t out_handle = -1;

// Input frames pulled from the ring in chunks (one lock per chunk).
static uint8_t in_chunk[1024 * 4];
static uint32_t in_frames = 0, in_pos = 0;

static bool next_frame(int16_t *l, int16_t *r) {
  if (in_pos >= in_frames) {
    uint32_t got = usbio_audio_pop(in_chunk, sizeof(in_chunk));
    in_frames = got / 4;
    in_pos = 0;
    if (in_frames == 0) {
      return false;
    }
  }
  const uint8_t *f = in_chunk + in_pos * 4;
  *l = (int16_t)(f[0] | (f[1] << 8));
  *r = (int16_t)(f[2] | (f[3] << 8));
  in_pos++;
  return true;
}

static int out_thread_fn(void *arg) {
  (void)arg;
  static int16_t frame_buf[OUT_GRANULARITY * 2];
  const double step = (double)IN_RATE / OUT_RATE;
  double pos = 1.0; // fractional position between prev and cur
  int16_t prev_l = 0, prev_r = 0, cur_l = 0, cur_r = 0;
  int prebuffering = 1;

  while (audio_running) {
    uint32_t level = usbio_audio_level();
    if (prebuffering && level >= PREBUFFER_BYTES) {
      prebuffering = 0;
      ps4_stage_once("audio: first samples playing");
    }
    if (!prebuffering && level > HIGH_WATER_BYTES) {
      // Producer clock runs fast relative to ours: drop back to the target.
      uint8_t scratch[1024];
      uint32_t excess = level - PREBUFFER_BYTES;
      while (excess >= 4) {
        uint32_t got = usbio_audio_pop(scratch, excess < sizeof(scratch) ? excess : sizeof(scratch));
        if (got == 0) {
          break;
        }
        excess -= got;
      }
      in_frames = in_pos = 0;
    }

    int f = 0;
    if (!prebuffering) {
      for (; f < OUT_GRANULARITY; f++) {
        while (pos >= 1.0) {
          prev_l = cur_l;
          prev_r = cur_r;
          if (!next_frame(&cur_l, &cur_r)) {
            goto starved;
          }
          pos -= 1.0;
        }
        frame_buf[f * 2 + 0] = (int16_t)(prev_l + (cur_l - prev_l) * pos);
        frame_buf[f * 2 + 1] = (int16_t)(prev_r + (cur_r - prev_r) * pos);
        pos += step;
      }
    }
    goto emit;
  starved:
    usbio_note_underrun();
    prebuffering = 1;
  emit:
    if (f < OUT_GRANULARITY) {
      memset(frame_buf + f * 2, 0, (OUT_GRANULARITY - f) * 4);
    }
    // Blocking: returns when the PREVIOUS buffer finished playing.
    if (sceAudioOutOutput(out_handle, frame_buf) < 0) {
      ps4_logf("audio: output failed");
      break;
    }
  }
  return 0;
}

int audio_init(unsigned int audio_buffer_size, const char *output_device_name) {
  (void)audio_buffer_size;
  (void)output_device_name;
  ps4_stage("audio: native init");

  if (audio_running) {
    return 1;
  }
  if (g_devh == NULL) {
    ps4_logf("audio: no M8 handle");
    return 0;
  }

  int rc = sceAudioOutInit();
  if (rc != 0) {
    // Already-initialised on a re-init is fine; Open below is the real test.
    ps4_logf("audio: sceAudioOutInit -> 0x%08X", rc);
  }
  if (out_handle <= 0) {
    out_handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
                                 0, OUT_GRANULARITY, OUT_RATE,
                                 ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (out_handle <= 0) {
      ps4_logf("audio: sceAudioOutOpen -> 0x%08X", out_handle);
      out_handle = -1;
      return 0;
    }
  }
  ps4_stage("audio: outopen ok");

  // From here the USB engine owns sceUsbd; the display path goes async too.
  if (!usbio_start(true)) {
    ps4_logf("audio: usbio start failed, staying display-only");
    return 0;
  }

  in_frames = in_pos = 0;
  audio_running = 1;
  out_thread = SDL_CreateThread(out_thread_fn, "m8c_audio_out", NULL);
  if (out_thread == NULL) {
    ps4_logf("audio: out thread create failed");
    audio_running = 0;
    return 0;
  }
  ps4_stage("audio: running");
  return 1;
}

void audio_destroy() {
  if (audio_running) {
    audio_running = 0;
    SDL_WaitThread(out_thread, NULL);
    out_thread = NULL;
  }
  // Every main.c call site is followed by (or follows) a disconnect.
  ps4_usb_quiesce();
  if (out_handle > 0) {
    sceAudioOutClose(out_handle);
    out_handle = -1;
  }
  ps4_logf("audio: closed");
}

void toggle_audio(unsigned int audio_buffer_size, const char *output_device_name) {
  (void)audio_buffer_size;
  (void)output_device_name;
  ps4_logf("audio: toggle not implemented");
}
