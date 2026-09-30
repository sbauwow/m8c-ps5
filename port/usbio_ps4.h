// Compat header for m8c-ps5: verbatim copy of the PS4 port's async-engine
// API (usbio_ps4.c is console-shared on purpose — same functions, same
// semantics, both consoles).
#ifndef USBIO_PS4_H_
#define USBIO_PS4_H_

#include <stdbool.h>
#include <stdint.h>

// Claims the audio interface (when with_audio), arms all transfers and starts
// the USB thread. Caller must not touch sceUsbd again until usbio_stop().
bool usbio_start(bool with_audio);

// Claims the audio interface and selects its streaming alt setting, WITHOUT
// arming anything. Must run before the first bulk transfer on the device:
// the alt switch kills the kernel pipes of endpoints already used (C6: every
// bulk OUT after it hung, while never-used bulk IN worked).
bool usbio_prepare_audio(void);

// Cancels everything, joins the USB thread, drops the audio alt setting.
// Idempotent. Queued writes are flushed first (bounded wait).
void usbio_stop(void);

bool usbio_active(void);

// Queue a CDC message for the M8. Returns len, or -1 when the engine is down.
int usbio_write(const uint8_t *buf, int len);

// Pop received CDC bytes. Returns count (0 when idle) or -1 if the M8 is gone.
int usbio_read(uint8_t *buf, int count);

// M8 PCM (44.1 kHz s16le stereo) for the native audio-out backend.
uint32_t usbio_audio_pop(uint8_t *buf, uint32_t len);
uint32_t usbio_audio_level(void);
void usbio_note_underrun(void);

#endif
