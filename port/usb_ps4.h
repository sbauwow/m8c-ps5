// Compat header for m8c-ps5: CDC backend contract shared with the PS4 port.
// The M8 constants and engine hooks are identical on both consoles; the only
// structural difference is the device-handle type (see ps5_usbd.h).
#ifndef USB_PS4_H_
#define USB_PS4_H_

#include <stdbool.h>
#include <stdint.h>

#include <libusb.h>

#include "ps5_usbd.h"

#define M8_VID 0x16c0
#define M8_PID 0x048a

#define ACM_CTRL_DTR 0x01
#define ACM_CTRL_RTS 0x02

// CDC endpoints (fixed on M8/Teensy, same as upstream usb.c)
#define EP_OUT 0x03
#define EP_IN 0x83

// M8 UAC2 audio capture. The interface is read from the descriptors at
// runtime (3 on fw 6.5.x); this is only the fallback if that fails.
#define AUDIO_IFACE_FALLBACK 3
#define AUDIO_ALT_SETTING 1
#define EP_ISO_IN 0x85

// Handle to the opened M8; NULL when no device. Owned by usb_ps5.c.
extern libusb_device_handle *g_devh;

// Loads libSceUsbd.sprx + sceUsbdInit exactly once; safe from any thread.
bool ps4_usbd_ensure_init(void);

// Stops the async engine if it runs, sending the M8 'D' through it first.
// Safe to call any number of times.
void ps4_usb_quiesce(void);

// Set from the config before the first init_serial: audio needs its
// interface configured before any bulk transfer.
void ps4_usb_set_audio_wanted(bool wanted);

#endif
