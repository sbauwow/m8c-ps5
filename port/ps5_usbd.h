// sceUsbd access for PS5 payloads, as a lazily-populated function table.
//
// The phase-0 probe proved on-console (2026-09-30): dlopen("libSceUsbd.sprx")
// works from a jailed elfldr payload and every symbol needed by this port
// resolves by NAME. The PS5 has no libSceUsbd stub library, so instead of
// link-time imports we bind at runtime — functionally identical to the PS4
// port, where sceUsbd* imports resolve at load time from the bundled sprx.
//
// Signatures follow the FreeBSD libusb-1.0 API (which both consoles' modules
// clone). Types come from the sysroot's real <libusb.h>; on PS5 the module
// matches upstream layouts, so no PS4-style hand-rolled structs are needed.
//
// The module is NEVER unloaded (sceUsbdExit is likewise never called — the
// PS4 port found module teardown can wedge).
#ifndef PS5_USBD_H_
#define PS5_USBD_H_

#include <libusb.h>

// libusb_transfer's ps4_* shim fields and the engine's field writes compile
// against this header; the engine (usbio_ps4.c) is console-shared.
#include <sys/time.h>

#define USBD_DECL(ret, name, params) typedef ret(*name##_fn) params;
#include "ps5_usbd_syms.inc"
#undef USBD_DECL

// The bound function pointers (defined in ps5_usbd.c).
#define USBD_DECL(ret, name, params) extern name##_fn name;
#include "ps5_usbd_syms.inc"
#undef USBD_DECL

// Loads libSceUsbd.sprx + resolves every symbol. Idempotent. Returns 0 on
// success; on failure every pointer stays NULL and the message is in the log.
int ps5_usbd_load(void);

// True once ps5_usbd_load() succeeded.
int ps5_usbd_ready(void);

#endif
