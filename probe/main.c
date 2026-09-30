// m8c-ps5 Phase 0 probe (PS5 11.60, kstuff/elfldr jailed payload).
//
// Answers, on real hardware, the questions that decide the m8c PS5 port
// architecture (mirrors ~/m8c-ps4/probe which cracked the PS4 side):
//   1. Does libSceUsbd.sprx load from a jailed payload (dlopen/dlsym, SDK rtld)?
//   2. Does the Dirtywave M8 / headless Teensy (16c0:048a) enumerate?
//   3. Do CDC-ACM control transfers + bulk write/read work (display enable +
//      firmware version read)?
//   4. Do the UAC2 interfaces parse, claim, and accept alt-setting switches?
//
// No iso transfers are armed here on purpose: transfer mechanics get proven
// by the real app's engine (usbio port) once base USB is green. This probe
// is deliberately hang-free: every USB call is bounded, teardown follows the
// PS4 lessons (alt 0 before release, no usbd exit, 'D' before close).
//
// Findings go to /data/m8c_ps5_probe.log, readable over ftpsrv FTP (2121).
// stdout goes nowhere; the log is the only channel.

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define M8_VID 0x16c0
#define M8_PID 0x048a

#define ACM_CTRL_DTR 0x01
#define ACM_CTRL_RTS 0x02

// ---------------------------------------------------------------------------
// minimal log (fflush after every line — a hang must leave the full answer
// up to the hang point readable over FTP)
static FILE *g_log = NULL;

static void logf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log ? g_log : stderr, fmt, ap);
  if (g_log ? g_log : stderr)
    fprintf(g_log ? g_log : stderr, "\n");
  va_end(ap);
  if (g_log) {
    fflush(g_log);
  }
}

static void hexlog(const char *what, const unsigned char *buf, int len) {
  logf("%s (%d bytes):", what, len);
  char line[80];
  for (int i = 0; i < len; i += 16) {
    int n = 0;
    for (int j = 0; j < 16 && i + j < len; j++) {
      n += snprintf(line + n, sizeof(line) - n, "%02X ", buf[i + j]);
    }
    logf("  %04X  %s", i, line);
  }
}

// ---------------------------------------------------------------------------
// sceUsbd function pointers (resolved via dlsym; signatures mirror libusb-1.0
// as proven on the PS4 clone — PS5 module is the same FreeBSD libusb lineage)
typedef struct usbd_device usbd_device;
typedef struct usbd_device_handle usbd_device_handle;

typedef int (*fn_usbd_init)(void);
typedef void (*fn_usbd_exit)(void);
typedef int (*fn_usbd_get_device_list)(usbd_device ***);
typedef void (*fn_usbd_free_device_list)(usbd_device **);
typedef int (*fn_usbd_get_device_descriptor)(usbd_device *, void *); // 18-byte std desc
typedef usbd_device_handle *(*fn_usbd_open_device_with_vid_pid)(uint16_t, uint16_t);
typedef int (*fn_usbd_close)(usbd_device_handle *);
typedef int (*fn_usbd_set_configuration)(usbd_device_handle *, int);
typedef int (*fn_usbd_claim_interface)(usbd_device_handle *, int);
typedef int (*fn_usbd_release_interface)(usbd_device_handle *, int);
typedef int (*fn_usbd_set_interface_alt_setting)(usbd_device_handle *, int, int);
typedef int (*fn_usbd_control_transfer)(usbd_device_handle *, uint8_t, uint8_t,
                                        uint16_t, uint16_t, unsigned char *,
                                        uint16_t, unsigned int);
typedef int (*fn_usbd_bulk_transfer)(usbd_device_handle *, unsigned char,
                                     unsigned char *, int, int *, unsigned int);
typedef int (*fn_usbd_get_active_config_descriptor)(usbd_device *, void *);
typedef void (*fn_usbd_free_config_descriptor)(void *);
typedef int (*fn_usbd_get_max_iso_packet_size)(usbd_device *, unsigned char);
typedef usbd_device *(*fn_usbd_get_device)(usbd_device_handle *);

static fn_usbd_init p_usbd_init;
static fn_usbd_exit p_usbd_exit;
static fn_usbd_get_device_list p_usbd_get_device_list;
static fn_usbd_free_device_list p_usbd_free_device_list;
static fn_usbd_get_device_descriptor p_usbd_get_device_descriptor;
static fn_usbd_open_device_with_vid_pid p_usbd_open_device_with_vid_pid;
static fn_usbd_close p_usbd_close;
static fn_usbd_set_configuration p_usbd_set_configuration;
static fn_usbd_claim_interface p_usbd_claim_interface;
static fn_usbd_release_interface p_usbd_release_interface;
static fn_usbd_set_interface_alt_setting p_usbd_set_interface_alt_setting;
static fn_usbd_control_transfer p_usbd_control_transfer;
static fn_usbd_bulk_transfer p_usbd_bulk_transfer;
static fn_usbd_get_active_config_descriptor p_usbd_get_active_config_descriptor;
static fn_usbd_free_config_descriptor p_usbd_free_config_descriptor;
static fn_usbd_get_max_iso_packet_size p_usbd_get_max_iso_packet_size;
static fn_usbd_get_device p_usbd_get_device;

static void *g_usbd = NULL;

static void *dlsym_log(const char *name) {
  void *p = dlsym(g_usbd, name);
  logf("dlsym(%s) -> %p", name, p);
  return p;
}

// ---------------------------------------------------------------------------
// raw config descriptor walk (layout-independent: no libusb struct layouts,
// the C4 lesson says never trust header structs across consoles)
static void parse_config_descriptor(const unsigned char *buf, int len) {
  int pos = 0;
  while (pos + 2 <= len) {
    int bLength = buf[pos];
    int bDescriptorType = buf[pos + 1];
    if (bLength < 2 || pos + bLength > len) {
      break;
    }
    if (bDescriptorType == 0x04 && bLength >= 9) { // interface
      logf("  IFACE num=%d alt=%d nEP=%d class=%02X sub=%02X proto=%02X",
           buf[pos + 2], buf[pos + 3], buf[pos + 4], buf[pos + 5],
           buf[pos + 6], buf[pos + 7]);
    } else if (bDescriptorType == 0x05 && bLength >= 7) { // endpoint
      int addr = buf[pos + 2];
      int attrs = buf[pos + 3];
      int maxpkt = buf[pos + 4] | (buf[pos + 5] << 8);
      const char *type = (attrs & 3) == 0   ? "ctrl"
                         : (attrs & 3) == 1 ? "iso"
                         : (attrs & 3) == 2 ? "bulk"
                                            : "intr";
      logf("    EP 0x%02X %s maxpkt=%d", addr, type, maxpkt);
    }
    pos += bLength;
  }
}

int main(void) {
  g_log = fopen("/data/m8c_ps5_probe.log", "w");
  if (!g_log) {
    return 1;
  }
  logf("=== m8c-ps5 probe start ===");

  // -- 1. load libSceUsbd ----------------------------------------------------
  logf("[1] dlopen(libSceUsbd.sprx, RTLD_LAZY)");
  g_usbd = dlopen("libSceUsbd.sprx", RTLD_LAZY);
  if (!g_usbd) {
    const char *e = dlerror();
    logf("  failed: %s", e ? e : "(no dlerror)");
    logf("[1b] retry dlopen(libSceUsbd, RTLD_LAZY)");
    g_usbd = dlopen("libSceUsbd", RTLD_LAZY);
    if (!g_usbd) {
      e = dlerror();
      logf("  failed: %s", e ? e : "(no dlerror)");
      logf("=== PROBE ABORT: libSceUsbd not loadable ===");
      fclose(g_log);
      return 2;
    }
  }
  logf("  handle=%p", g_usbd);

  p_usbd_init = (fn_usbd_init)dlsym_log("sceUsbdInit");
  p_usbd_exit = (fn_usbd_exit)dlsym_log("sceUsbdExit");
  p_usbd_get_device_list = (fn_usbd_get_device_list)dlsym_log("sceUsbdGetDeviceList");
  p_usbd_free_device_list = (fn_usbd_free_device_list)dlsym_log("sceUsbdFreeDeviceList");
  p_usbd_get_device_descriptor =
      (fn_usbd_get_device_descriptor)dlsym_log("sceUsbdGetDeviceDescriptor");
  p_usbd_open_device_with_vid_pid =
      (fn_usbd_open_device_with_vid_pid)dlsym_log("sceUsbdOpenDeviceWithVidPid");
  p_usbd_close = (fn_usbd_close)dlsym_log("sceUsbdClose");
  p_usbd_set_configuration = (fn_usbd_set_configuration)dlsym_log("sceUsbdSetConfiguration");
  p_usbd_claim_interface = (fn_usbd_claim_interface)dlsym_log("sceUsbdClaimInterface");
  p_usbd_release_interface = (fn_usbd_release_interface)dlsym_log("sceUsbdReleaseInterface");
  p_usbd_set_interface_alt_setting =
      (fn_usbd_set_interface_alt_setting)dlsym_log("sceUsbdSetInterfaceAltSetting");
  p_usbd_control_transfer = (fn_usbd_control_transfer)dlsym_log("sceUsbdControlTransfer");
  p_usbd_bulk_transfer = (fn_usbd_bulk_transfer)dlsym_log("sceUsbdBulkTransfer");
  p_usbd_get_active_config_descriptor =
      (fn_usbd_get_active_config_descriptor)dlsym_log("sceUsbdGetActiveConfigDescriptor");
  p_usbd_free_config_descriptor =
      (fn_usbd_free_config_descriptor)dlsym_log("sceUsbdFreeConfigDescriptor");
  p_usbd_get_max_iso_packet_size =
      (fn_usbd_get_max_iso_packet_size)dlsym_log("sceUsbdGetMaxIsoPacketSize");
  p_usbd_get_device = (fn_usbd_get_device)dlsym_log("sceUsbdGetDevice");

  if (!p_usbd_init || !p_usbd_get_device_list || !p_usbd_open_device_with_vid_pid) {
    logf("=== PROBE ABORT: core symbols missing ===");
    fclose(g_log);
    return 3;
  }

  // -- 2. init + enumerate ---------------------------------------------------
  logf("[2] sceUsbdInit -> %d", p_usbd_init());

  usbd_device **list = NULL;
  int count = p_usbd_get_device_list(&list);
  logf("device count: %d", count);
  int m8_index = -1;
  if (count > 0 && list) {
    for (int i = 0; i < count; i++) {
      unsigned char d[18];
      memset(d, 0, sizeof(d));
      if (p_usbd_get_device_descriptor(list[i], d) < 0) {
        logf("  [%d] <descriptor failed>", i);
        continue;
      }
      int vid = d[8] | (d[9] << 8);
      int pid = d[10] | (d[11] << 8);
      logf("  [%d] %04X:%04X class=%02X", i, vid, pid, d[4]);
      if (vid == M8_VID && pid == M8_PID) {
        m8_index = i;
      }
    }
    p_usbd_free_device_list(list);
  }
  if (m8_index < 0) {
    logf("=== PROBE DONE: no M8 found (plug it in and re-run) ===");
    fclose(g_log);
    return 4;
  }

  // -- 3. open + CDC handshake ----------------------------------------------
  logf("[3] OpenDeviceWithVidPid(%04X,%04X)", M8_VID, M8_PID);
  usbd_device_handle *devh = p_usbd_open_device_with_vid_pid(M8_VID, M8_PID);
  if (!devh) {
    logf("  open failed");
    logf("=== PROBE DONE: M8 visible but cannot open (kernel driver hold?) ===");
    fclose(g_log);
    return 5;
  }
  logf("  devh=%p", (void *)devh);

  int rc = p_usbd_set_configuration(devh, 1);
  logf("SetConfiguration(1) -> %d", rc);

  for (int iface = 0; iface <= 1; iface++) {
    rc = p_usbd_claim_interface(devh, iface);
    logf("ClaimInterface(%d) -> %d", iface, rc);
    if (rc < 0) {
      goto teardown;
    }
  }

  rc = p_usbd_control_transfer(devh, 0x21, 0x22, ACM_CTRL_DTR | ACM_CTRL_RTS, 0,
                               NULL, 0, 500);
  logf("SET_CONTROL_LINE_STATE -> %d", rc);

  {
    unsigned char encoding[] = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08}; // 115200 8N1
    rc = p_usbd_control_transfer(devh, 0x21, 0x20, 0, 0, encoding,
                                 sizeof(encoding), 500);
    logf("SET_LINE_ENCODING -> %d", rc);
  }

  // display enable + reset, then read the fw version packet
  {
    unsigned char cmd = 'E';
    int sent = 0;
    rc = p_usbd_bulk_transfer(devh, 0x03, &cmd, 1, &sent, 500);
    logf("bulk OUT 'E' -> %d (sent %d)", rc, sent);
    if (rc >= 0 && sent == 1) {
      unsigned char cmdr = 'R';
      rc = p_usbd_bulk_transfer(devh, 0x03, &cmdr, 1, &sent, 500);
      logf("bulk OUT 'R' -> %d (sent %d)", rc, sent);

      unsigned char rx[64];
      int got = 0;
      memset(rx, 0, sizeof(rx));
      rc = p_usbd_bulk_transfer(devh, 0x83, rx, sizeof(rx), &got, 1000);
      logf("bulk IN 0x83 -> %d (got %d)", rc, got);
      if (got > 0) {
        hexlog("M8 reply", rx, got);
      }
    }
  }

  // -- 4. UAC2 interfaces ----------------------------------------------------
  logf("[4] active config descriptor:");
  if (p_usbd_get_active_config_descriptor && p_usbd_free_config_descriptor) {
    usbd_device *dev = p_usbd_get_device ? p_usbd_get_device(devh) : NULL;
    void *cfg = NULL;
    // active-config descriptor takes the device (PS4 lesson); fall back to devh
    if (p_usbd_get_active_config_descriptor(dev ? dev : (usbd_device *)devh, &cfg) == 0 && cfg) {
      // libusb's parsed struct starts: bLength,bDescriptorType,wTotalLength(2),
      // bNumInterfaces,bConfigurationValue,... then packed interface array.
      // Walk raw bytes instead: grab wTotalLength from the struct head and
      // re-fetch raw via control transfer for layout independence.
      int total = ((unsigned char *)cfg)[2] | (((unsigned char *)cfg)[3] << 8);
      logf("  wTotalLength=%d", total);
      p_usbd_free_config_descriptor(cfg);

      unsigned char raw[1024];
      memset(raw, 0, sizeof(raw));
      int n = (total > (int)sizeof(raw)) ? (int)sizeof(raw) : total;
      int got = 0;
      rc = p_usbd_control_transfer(devh, 0x80, 0x06, 0x0200, 0, raw, n, 500);
      logf("GET_DESCRIPTOR(CONFIG) -> %d (got %d)", rc, got);
      if (rc == n) {
        parse_config_descriptor(raw, n);
      }
    } else {
      logf("  GetActiveConfigDescriptor failed");
    }
  }

  for (int iface = 2; iface <= 4; iface++) {
    rc = p_usbd_claim_interface(devh, iface);
    logf("ClaimInterface(%d) -> %d", iface, rc);
  }

  // capture iface alt 1 (fw 6.5.x: iface 3 alt 1, EP 0x85) — claim state from
  // the log above decides which iface number really owns 0x85 on the PS5
  for (int iface = 2; iface <= 4; iface++) {
    rc = p_usbd_set_interface_alt_setting(devh, iface, 1);
    logf("SetInterfaceAltSetting(%d,1) -> %d", iface, rc);
  }

  if (p_usbd_get_max_iso_packet_size) {
    usbd_device *dev = p_usbd_get_device ? p_usbd_get_device(devh) : NULL;
    if (dev) {
      rc = p_usbd_get_max_iso_packet_size(dev, 0x85);
      logf("GetMaxIsoPacketSize(dev,0x85) -> %d", rc);
    }
    // PS4 took device not handle; log both spellings for the record
    rc = p_usbd_get_max_iso_packet_size((usbd_device *)devh, 0x85);
    logf("GetMaxIsoPacketSize(devh,0x85) -> %d", rc);
  }

  // -- teardown (PS4 lessons: alt 0 BEFORE release, 'D' before close) --------
teardown:
  for (int iface = 4; iface >= 0; iface--) {
    int rc2 = p_usbd_set_interface_alt_setting(devh, iface, 0);
    if (rc2 != 0) {
      logf("alt(%d,0) -> %d", iface, rc2);
    }
    rc2 = p_usbd_release_interface(devh, iface);
    if (rc2 != 0) {
      logf("release(%d) -> %d", iface, rc2);
    }
  }
  {
    unsigned char cmd = 'D';
    int sent = 0;
    rc = p_usbd_bulk_transfer(devh, 0x03, &cmd, 1, &sent, 500);
    logf("bulk OUT 'D' -> %d (sent %d)", rc, sent);
  }
  rc = p_usbd_close(devh);
  logf("Close -> %d", rc);

  // deliberately no sceUsbdExit (module teardown wedged on the PS4)
  logf("=== PROBE COMPLETE ===");
  fclose(g_log);
  return 0;
}
