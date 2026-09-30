// Async USB engine (see usbio_ps4.h for the why).
// PS5 port: VERBATIM copy of port/usbio_ps4.c (C7, console-proven) except
// for the include block and the sceUsbd name bindings below.
//
// Three OpenOrbis Usbd.h mismatches with libusb 1.0 (which sceUsbd clones)
// are worked around here; they are the prime suspects for the "event
// servicing deadlocks / crashes" findings in notes/RECON.md:
//   1. sceUsbdHandleEventsTimeout is declared (int32_t *) but libusb takes a
//      struct timeval * (16 bytes on amd64). Passing an int32 made the module
//      read 12 bytes of stack garbage as the timeout. We pass a real timeval;
//      its first 4 bytes are the low half of tv_sec (0), so this is also
//      harmless if the int32 prototype were right.
//   2. sceUsbdFillIsoTransfer is declared without libusb's num_iso_packets
//      argument, so every later argument may be shifted. We never call the
//      Fill* helpers: the transfer struct is public, we set its fields.
//   3. The old code set type=3 (iso) / type=1 (bulk). libusb's enum is
//      CONTROL=0, ISOCHRONOUS=1, BULK=2, INTERRUPT=3.

#include <stdio.h>
#include <string.h>

// Bind the module's exports (bound at runtime on PS5 — no stub library).
// MUST come before the includes below (see ps5_usbd.c for the ordering note).
#include "ps5_usbd_names.h"

#include <SDL.h>
#include <SDL_thread.h>

#include <sys/time.h>

#include "ps4_shims.h"
#include "ps5_usbd.h"
#include "ringbuffer.h"
#include "usb_ps4.h"
#include "usbio_ps4.h"

#define XFER_TYPE_ISO 1
#define XFER_TYPE_BULK 2

#define RX_XFERS 2
#define RX_LEN 1024
#define TX_QUEUE 64
#define TX_MSG_MAX 4
#define MAX_ISO_XFERS 32
#define MAX_ISO_PKTS 64
#define RX_RING_BYTES (64 * 1024)
#define AUDIO_RING_BYTES (256 * 1024)
#define STATS_EVERY_MS 5000
#define WATCHDOG_STALL_MS 2000
#define OVERRIDES_PATH "/data/m8c_audio.ini"

// libusb_endpoint_descriptor: the toolchain header leaves it incomplete.
struct ep_desc {
  uint8_t bLength;
  uint8_t bDescriptorType;
  uint8_t bEndpointAddress;
  uint8_t bmAttributes;
  uint16_t wMaxPacketSize;
  uint8_t bInterval;
  uint8_t bRefresh;
  uint8_t bSynchAddress;
  const unsigned char *extra;
  int extra_length;
};

// struct timeval on FreeBSD amd64.
struct usbd_timeval {
  int64_t tv_sec;
  int64_t tv_usec;
};

// Tunables; defaults may be replaced from OVERRIDES_PATH (key=value lines).
static int iso_pkt = 0; // 0 = pick from the stack/descriptor at start
static int iso_npkts = 16;
static int iso_nxfers = 16;
static int ev_us = 1000;
static int tx_timeout = 0; // 0 = none; C5's 200 ms send never completed
static volatile int tx_sync = 0; // 1: blocking bulk OUT from the USB thread

static SDL_Thread *usb_thread = NULL;
static SDL_Thread *dog_thread = NULL;
static volatile int running = 0;
static volatile int stopping = 0;
static volatile int dev_gone = 0;
static bool audio_claimed = false;
static int audio_iface = -1; // interface whose audio alt carries EP_ISO_IN

// Only touched by whichever thread currently owns sceUsbd.
static int outstanding = 0;

static SDL_mutex *rx_lock = NULL;
static SDL_mutex *audio_lock = NULL;
static SDL_mutex *tx_lock = NULL;
static RingBuffer *rx_ring = NULL;
static RingBuffer *audio_ring = NULL;

static struct libusb_transfer *rx_xfr[RX_XFERS];
static uint8_t rx_buf[RX_XFERS][RX_LEN];
static struct libusb_transfer *iso_xfr[MAX_ISO_XFERS];
static uint8_t *iso_buf[MAX_ISO_XFERS];
static struct libusb_transfer *tx_xfr = NULL;
static uint8_t tx_buf[TX_MSG_MAX];
static volatile int tx_busy = 0;
static uint32_t tx_started = 0;
static int tx_stall_stage = 0; // 0 ok, 1 cancel requested

static struct {
  uint8_t len;
  uint8_t b[TX_MSG_MAX];
} tx_q[TX_QUEUE];
static int tx_head = 0, tx_count = 0;

static struct {
  uint32_t rx_bytes, rx_xfers, rx_err;
  uint32_t tx_msgs, tx_err, tx_drop;
  uint32_t iso_xfers, iso_bytes, iso_pk_ok, iso_pk_zero, iso_max_len;
  uint32_t iso_status[8];
  uint32_t iso_xfer_err, audio_drop, underruns, ev_err;
  uint32_t ev_calls, ev_max_ms, ev_slow; // per window; slow = >= 20 ms
  uint32_t tx_stalls;
} st;

static volatile uint32_t usb_beat = 0;
static volatile const char *usb_where = "init";
static volatile uint32_t main_beat = 0;

static void fill_xfer(struct libusb_transfer *t, unsigned char ep, unsigned char type,
                      uint8_t *buf, int len, libusb_transfer_cb_fn cb, unsigned int timeout) {
  t->dev_handle = g_devh;
  t->flags = 0;
  t->endpoint = ep;
  t->type = type;
  t->timeout = timeout;
  t->length = len;
  t->callback = cb;
  t->user_data = NULL;
  t->buffer = buf;
}

static int submit(struct libusb_transfer *t) {
  int rc = sceUsbdSubmitTransfer(t);
  if (rc >= 0) {
    outstanding++;
  }
  return rc;
}

// Completion callbacks run on the USB thread, inside HandleEventsTimeout.
// A callback either resubmits (transfer stays outstanding) or retires it.

static void rx_cb(struct libusb_transfer *t) {
  outstanding--;
  if (t->status == LIBUSB_TRANSFER_CANCELLED) {
    return;
  }
  if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
    dev_gone = 1;
    return;
  }
  if (t->status == LIBUSB_TRANSFER_COMPLETED || t->status == LIBUSB_TRANSFER_TIMED_OUT) {
    st.rx_xfers++;
    if (t->actual_length > 0) {
      st.rx_bytes += t->actual_length;
      SDL_LockMutex(rx_lock);
      ring_buffer_push(rx_ring, t->buffer, t->actual_length);
      SDL_UnlockMutex(rx_lock);
    }
  } else if (st.rx_err++ < 5) {
    ps4_logf("usbio: rx status %d", t->status);
  }
  if (!stopping && submit(t) < 0) {
    ps4_logf("usbio: rx resubmit failed");
    dev_gone = 1;
  }
}

static void tx_cb(struct libusb_transfer *t) {
  outstanding--;
  tx_busy = 0;
  if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
    dev_gone = 1;
  } else if (t->status != LIBUSB_TRANSFER_COMPLETED && t->status != LIBUSB_TRANSFER_CANCELLED) {
    if (st.tx_err++ < 5) {
      ps4_logf("usbio: tx status %d", t->status);
    }
  }
}

static void iso_cb(struct libusb_transfer *t) {
  outstanding--;
  if (t->status == LIBUSB_TRANSFER_CANCELLED) {
    return;
  }
  if (t->status == LIBUSB_TRANSFER_NO_DEVICE) {
    dev_gone = 1;
    return;
  }
  if (t->status != LIBUSB_TRANSFER_COMPLETED) {
    if (st.iso_xfer_err++ < 5) {
      ps4_logf("usbio: iso xfer status %d len %d, pkt0 status %d len %u", t->status,
               t->actual_length, t->iso_packet_desc[0].status, t->iso_packet_desc[0].actual_length);
    }
  } else {
    st.iso_xfers++;
    for (int i = 0; i < t->num_iso_packets; i++) {
      struct libusb_iso_packet_descriptor *p = &t->iso_packet_desc[i];
      st.iso_status[p->status & 7]++;
      if (p->status != LIBUSB_TRANSFER_COMPLETED) {
        continue;
      }
      uint32_t n = p->actual_length;
      if (n == 0) {
        st.iso_pk_zero++;
        continue;
      }
      if (n > (uint32_t)iso_pkt) {
        n = iso_pkt;
      }
      st.iso_pk_ok++;
      st.iso_bytes += n;
      if (n > st.iso_max_len) {
        st.iso_max_len = n;
      }
      SDL_LockMutex(audio_lock);
      uint32_t pushed = ring_buffer_push(audio_ring, t->buffer + i * iso_pkt, n);
      SDL_UnlockMutex(audio_lock);
      if (pushed != n) {
        st.audio_drop++;
      }
    }
  }
  if (!stopping && submit(t) < 0) {
    if (st.iso_xfer_err++ < 5) {
      ps4_logf("usbio: iso resubmit failed");
    }
  }
}

// An async send that never completes blocks every later message (C5: input
// lost). Cancel it; if even the cancel never completes, abandon the
// transfer and fall back to blocking sends on this thread.
static void check_tx_stall(void) {
  uint32_t age = SDL_GetTicks() - tx_started;
  if (tx_stall_stage == 0 && age > 500) {
    st.tx_stalls++;
    ps4_logf("usbio: tx stuck %ums: status %d actual %d len %d, cancelling", age, tx_xfr->status,
             tx_xfr->actual_length, tx_xfr->length);
    ps4_logf("usbio: tx cancel -> 0x%08X", sceUsbdCancelTransfer(tx_xfr));
    tx_stall_stage = 1;
  } else if (tx_stall_stage == 1 && age > 1000) {
    ps4_logf("usbio: tx cancel never completed; switching to sync sends");
    tx_xfr = NULL; // leaked on purpose: the module may still own it
    outstanding--;
    tx_busy = 0;
    tx_sync = 1;
    tx_stall_stage = 0;
  }
}

static void service_tx(void) {
  if (tx_busy) {
    check_tx_stall();
    return;
  }
  SDL_LockMutex(tx_lock);
  if (tx_count == 0) {
    SDL_UnlockMutex(tx_lock);
    return;
  }
  uint8_t len = tx_q[tx_head].len;
  memcpy(tx_buf, tx_q[tx_head].b, len);
  tx_head = (tx_head + 1) % TX_QUEUE;
  tx_count--;
  SDL_UnlockMutex(tx_lock);

  if (tx_sync) {
    static int logged = 0;
    int sent = 0;
    usb_where = "tx-sync";
    int rc = sceUsbdBulkTransfer(g_devh, EP_OUT, tx_buf, len, &sent, 50);
    usb_where = "tx";
    if (!logged) {
      ps4_logf("usbio: first sync send -> 0x%08X sent %d", rc, sent);
      logged = 1;
    }
    if (rc < 0 || sent != len) {
      st.tx_err++;
    } else {
      st.tx_msgs++;
    }
    return;
  }

  fill_xfer(tx_xfr, EP_OUT, XFER_TYPE_BULK, tx_buf, len, tx_cb, tx_timeout);
  tx_busy = 1;
  tx_started = SDL_GetTicks();
  tx_stall_stage = 0;
  if (submit(tx_xfr) < 0) {
    tx_busy = 0;
    st.tx_err++;
    return;
  }
  st.tx_msgs++;
}

static int handle_events(int usec) {
  struct usbd_timeval tv = {0, usec};
  uint32_t t0 = SDL_GetTicks();
  // C4 lesson: the module takes libusb's 16-byte timeval. OpenOrbis's header
  // declared int32_t*, hence the old cast; the PS5 declaration here is the
  // true signature, and usbd_timeval is layout-identical to struct timeval.
  int rc = sceUsbdHandleEventsTimeout((struct timeval *)&tv);
  uint32_t dt = SDL_GetTicks() - t0;
  st.ev_calls++;
  if (dt > st.ev_max_ms) {
    st.ev_max_ms = dt;
  }
  if (dt >= 20) {
    st.ev_slow++;
  }
  if (rc < 0 && st.ev_err++ < 5) {
    ps4_logf("usbio: HandleEventsTimeout -> 0x%08X", rc);
  }
  return rc;
}

static void log_stats(uint32_t window_ms) {
  uint32_t ring;
  SDL_LockMutex(audio_lock);
  ring = audio_ring ? audio_ring->size : 0;
  SDL_UnlockMutex(audio_lock);
  ps4_logf("usbio: rx %uB/%uxf err %u | tx %u err %u drop %u stalls %u %s | iso %uxf %uB/s ok %u zero %u "
           "max %u st[%u %u %u %u %u %u %u] xerr %u | ring %u drop %u under %u | ev %u calls max %ums "
           "slow %u err %u | usb in '%s'",
           st.rx_bytes, st.rx_xfers, st.rx_err, st.tx_msgs, st.tx_err, st.tx_drop, st.tx_stalls,
           tx_sync ? "sync" : (tx_busy ? "busy" : "idle"), st.iso_xfers,
           (uint32_t)((uint64_t)st.iso_bytes * 1000 / (window_ms ? window_ms : 1)), st.iso_pk_ok,
           st.iso_pk_zero, st.iso_max_len, st.iso_status[0], st.iso_status[1], st.iso_status[2],
           st.iso_status[3], st.iso_status[4], st.iso_status[5], st.iso_status[6],
           st.iso_xfer_err, ring, st.audio_drop, st.underruns, st.ev_calls, st.ev_max_ms, st.ev_slow,
           st.ev_err, (const char *)usb_where);
  // Rate counters are per window; error counters stay cumulative.
  st.iso_bytes = 0;
  st.ev_calls = st.ev_max_ms = st.ev_slow = 0;
}

static int usb_thread_fn(void *arg) {
  (void)arg;
  ps4_logf("usbio: thread up, %d armed", outstanding);

  while (!stopping) {
    usb_beat = SDL_GetTicks();
    usb_where = "events";
    handle_events(ev_us);
    usb_where = "tx";
    service_tx();
  }

  // Retire everything. Never call HandleEvents with nothing armed (it
  // crashed the module in the old build), hence the outstanding guard.
  usb_where = "cancel";
  for (int i = 0; i < RX_XFERS; i++) {
    if (rx_xfr[i] != NULL) {
      sceUsbdCancelTransfer(rx_xfr[i]);
    }
  }
  for (int i = 0; i < iso_nxfers; i++) {
    if (iso_xfr[i] != NULL) {
      sceUsbdCancelTransfer(iso_xfr[i]);
    }
  }
  if (tx_busy && tx_xfr != NULL) {
    sceUsbdCancelTransfer(tx_xfr);
  }
  for (int i = 0; outstanding > 0 && i < 500; i++) {
    usb_beat = SDL_GetTicks();
    handle_events(2000);
  }
  ps4_logf("usbio: thread down, %d still outstanding", outstanding);
  usb_where = "down";
  return 0;
}

static int dog_thread_fn(void *arg) {
  (void)arg;
  int usb_reported = 0, main_reported = 0;
  uint32_t last_stats = SDL_GetTicks();
  while (running) {
    SDL_Delay(500);
    uint32_t now = SDL_GetTicks();
    // Logged from here, not the USB thread: a stuck USB thread still reports.
    if (now - last_stats >= STATS_EVERY_MS) {
      log_stats(now - last_stats);
      last_stats = now;
    }
    if (now - usb_beat > WATCHDOG_STALL_MS) {
      if (!usb_reported) {
        ps4_logf("WATCHDOG: usb thread stuck in '%s' for %ums", (const char *)usb_where,
                 now - usb_beat);
        usb_reported = 1;
      }
    } else {
      usb_reported = 0;
    }
    if (main_beat != 0 && now - main_beat > WATCHDOG_STALL_MS) {
      if (!main_reported) {
        ps4_logf("WATCHDOG: main thread has not read serial for %ums", now - main_beat);
        main_reported = 1;
      }
    } else {
      main_reported = 0;
    }
  }
  return 0;
}

static void load_overrides(void) {
  FILE *f = fopen(OVERRIDES_PATH, "r");
  if (f == NULL) {
    return;
  }
  char line[128];
  int v;
  while (fgets(line, sizeof(line), f) != NULL) {
    if (sscanf(line, "pkt=%d", &v) == 1) {
      iso_pkt = v;
    } else if (sscanf(line, "npkts=%d", &v) == 1) {
      iso_npkts = v;
    } else if (sscanf(line, "nxfers=%d", &v) == 1) {
      iso_nxfers = v;
    } else if (sscanf(line, "ev_us=%d", &v) == 1) {
      ev_us = v;
    } else if (sscanf(line, "tx_timeout=%d", &v) == 1) {
      tx_timeout = v;
    } else if (strncmp(line, "tx=sync", 7) == 0) {
      tx_sync = 1;
    }
  }
  fclose(f);
  if (iso_npkts < 1 || iso_npkts > MAX_ISO_PKTS) {
    iso_npkts = 16;
  }
  if (iso_nxfers < 1 || iso_nxfers > MAX_ISO_XFERS) {
    iso_nxfers = 16;
  }
  ps4_logf("usbio: overrides pkt=%d npkts=%d nxfers=%d ev_us=%d tx_timeout=%d tx_sync=%d",
           iso_pkt, iso_npkts, iso_nxfers, ev_us, tx_timeout, tx_sync);
}

// Logs every interface/alt/endpoint; returns EP_ISO_IN's size in the audio
// alt setting (wMaxPacketSize x mult), 0 if not found.
static int dump_descriptors(libusb_device *dev) {
  int iso_size = 0;
  ps4_logf("usbio: device speed %d", sceUsbdGetDeviceSpeed(dev));
  struct libusb_config_descriptor *cfg = NULL;
  if (sceUsbdGetActiveConfigDescriptor(dev, &cfg) < 0 || cfg == NULL) {
    ps4_logf("usbio: no active config descriptor");
    return 0;
  }
  for (int i = 0; i < cfg->bNumInterfaces; i++) {
    const struct libusb_interface *itf = &cfg->interface[i];
    for (int a = 0; a < itf->num_altsetting; a++) {
      const struct libusb_interface_descriptor *alt = &itf->altsetting[a];
      ps4_logf("usbio: if %d alt %d class %02x/%02x eps %d", alt->bInterfaceNumber,
               alt->bAlternateSetting, alt->bInterfaceClass, alt->bInterfaceSubClass,
               alt->bNumEndpoints);
      const struct ep_desc *eps = (const struct ep_desc *)alt->endpoint;
      for (int e = 0; e < alt->bNumEndpoints; e++) {
        const struct ep_desc *ep = &eps[e];
        ps4_logf("usbio:   ep %02x attr %02x maxpkt 0x%04x interval %d", ep->bEndpointAddress,
                 ep->bmAttributes, ep->wMaxPacketSize, ep->bInterval);
        // Upstream m8c hardcodes interface 4, but on fw 6.5.x EP 0x85 lives
        // on interface 3 (4 is the host->M8 stream, EP 0x05 + feedback).
        if (alt->bAlternateSetting == AUDIO_ALT_SETTING && ep->bEndpointAddress == EP_ISO_IN) {
          audio_iface = alt->bInterfaceNumber;
          iso_size = (ep->wMaxPacketSize & 0x7FF) * (((ep->wMaxPacketSize >> 11) & 3) + 1);
        }
      }
    }
  }
  sceUsbdFreeConfigDescriptor(cfg);
  return iso_size;
}

static void load_overrides(void);

static bool setup_audio_iface(void) {
  libusb_device *dev = sceUsbdGetDevice(g_devh);
  int desc_pkt = dump_descriptors(dev);
  if (audio_iface < 0) {
    audio_iface = AUDIO_IFACE_FALLBACK;
  }
  ps4_logf("usbio: audio capture interface %d", audio_iface);
  int stack_before = sceUsbdGetMaxIsoPacketSize(dev, EP_ISO_IN);

  int rc = sceUsbdClaimInterface(g_devh, audio_iface);
  if (rc < 0) {
    ps4_logf("usbio: claim(%d) -> 0x%08X", audio_iface, rc);
    return false;
  }
  rc = sceUsbdSetInterfaceAltSetting(g_devh, audio_iface, AUDIO_ALT_SETTING);
  if (rc < 0) {
    ps4_logf("usbio: altset(%d,%d) -> 0x%08X", audio_iface, AUDIO_ALT_SETTING, rc);
    sceUsbdReleaseInterface(g_devh, audio_iface);
    return false;
  }
  audio_claimed = true;
  int stack_after = sceUsbdGetMaxIsoPacketSize(dev, EP_ISO_IN);

  int chosen = iso_pkt > 0 ? iso_pkt : stack_after > 0 ? stack_after : desc_pkt > 0 ? desc_pkt : 180;
  ps4_logf("usbio: iso pkt desc=%d stack_before=%d stack_after=%d override=%d -> %d", desc_pkt,
           stack_before, stack_after, iso_pkt, chosen);
  iso_pkt = chosen;
  return true;
}

static void release_audio_iface(void) {
  if (!audio_claimed) {
    return;
  }
  sceUsbdSetInterfaceAltSetting(g_devh, audio_iface, 0);
  sceUsbdReleaseInterface(g_devh, audio_iface);
  audio_claimed = false;
}

static void free_transfers(void) {
  if (outstanding > 0) {
    // Freeing an in-flight transfer is a use-after-free in the module.
    ps4_logf("usbio: leaking transfers, %d outstanding", outstanding);
    return;
  }
  for (int i = 0; i < RX_XFERS; i++) {
    if (rx_xfr[i] != NULL) {
      sceUsbdFreeTransfer(rx_xfr[i]);
      rx_xfr[i] = NULL;
    }
  }
  for (int i = 0; i < MAX_ISO_XFERS; i++) {
    if (iso_xfr[i] != NULL) {
      sceUsbdFreeTransfer(iso_xfr[i]);
      iso_xfr[i] = NULL;
    }
    SDL_free(iso_buf[i]);
    iso_buf[i] = NULL;
  }
  if (tx_xfr != NULL) {
    sceUsbdFreeTransfer(tx_xfr);
    tx_xfr = NULL;
  }
}

// Undo a partial start on the caller's thread.
static void abort_start(void) {
  stopping = 1;
  for (int i = 0; i < RX_XFERS; i++) {
    if (rx_xfr[i] != NULL) {
      sceUsbdCancelTransfer(rx_xfr[i]);
    }
  }
  for (int i = 0; i < MAX_ISO_XFERS; i++) {
    if (iso_xfr[i] != NULL) {
      sceUsbdCancelTransfer(iso_xfr[i]);
    }
  }
  for (int i = 0; outstanding > 0 && i < 500; i++) {
    handle_events(2000);
  }
  free_transfers();
  release_audio_iface();
}

bool usbio_start(bool with_audio) {
  if (running || g_devh == NULL) {
    return running;
  }
  ps4_stage("usbio: start");
  memset(&st, 0, sizeof(st));
  stopping = 0;
  dev_gone = 0;
  outstanding = 0;
  tx_head = tx_count = 0;
  tx_busy = 0;
  tx_sync = 0;
  tx_stall_stage = 0;
  load_overrides();

  if (rx_lock == NULL) {
    rx_lock = SDL_CreateMutex();
    audio_lock = SDL_CreateMutex();
    tx_lock = SDL_CreateMutex();
    rx_ring = ring_buffer_create(RX_RING_BYTES);
    audio_ring = ring_buffer_create(AUDIO_RING_BYTES);
  }
  rx_ring->head = rx_ring->tail = rx_ring->size = 0;
  audio_ring->head = audio_ring->tail = audio_ring->size = 0;

  if (with_audio && !audio_claimed && !setup_audio_iface()) {
    return false;
  }

  // Bulk transfers use alloc(1): the old build found alloc(0) unusable.
  tx_xfr = sceUsbdAllocTransfer(1);
  if (tx_xfr == NULL) {
    ps4_logf("usbio: tx alloc failed");
    abort_start();
    return false;
  }
  tx_xfr->num_iso_packets = 0;

  ps4_stage("usbio: arm rx");
  for (int i = 0; i < RX_XFERS; i++) {
    rx_xfr[i] = sceUsbdAllocTransfer(1);
    if (rx_xfr[i] == NULL) {
      ps4_logf("usbio: rx alloc failed");
      abort_start();
      return false;
    }
    fill_xfer(rx_xfr[i], EP_IN, XFER_TYPE_BULK, rx_buf[i], RX_LEN, rx_cb, 0);
    rx_xfr[i]->num_iso_packets = 0;
    int rc = submit(rx_xfr[i]);
    if (rc < 0) {
      ps4_logf("usbio: rx submit %d -> 0x%08X", i, rc);
      abort_start();
      return false;
    }
  }

  if (with_audio) {
    ps4_stage("usbio: arm iso");
    for (int i = 0; i < iso_nxfers; i++) {
      iso_xfr[i] = sceUsbdAllocTransfer(iso_npkts);
      iso_buf[i] = SDL_malloc(iso_pkt * iso_npkts);
      if (iso_xfr[i] == NULL || iso_buf[i] == NULL) {
        ps4_logf("usbio: iso alloc %d failed", i);
        abort_start();
        return false;
      }
      struct libusb_transfer *t = iso_xfr[i];
      fill_xfer(t, EP_ISO_IN, XFER_TYPE_ISO, iso_buf[i], iso_pkt * iso_npkts, iso_cb, 0);
      t->num_iso_packets = iso_npkts;
      for (int p = 0; p < iso_npkts; p++) {
        t->iso_packet_desc[p].length = iso_pkt;
      }
      int rc = submit(t);
      if (rc < 0) {
        ps4_logf("usbio: iso submit %d -> 0x%08X", i, rc);
        abort_start();
        return false;
      }
    }
  }

  usb_beat = SDL_GetTicks();
  main_beat = 0;
  running = 1;
  usb_thread = SDL_CreateThread(usb_thread_fn, "m8c_usb", NULL);
  if (usb_thread == NULL) {
    ps4_logf("usbio: thread create failed");
    running = 0;
    abort_start();
    return false;
  }
  dog_thread = SDL_CreateThread(dog_thread_fn, "m8c_watchdog", NULL);
  ps4_stage("usbio: running");
  return true;
}

void usbio_stop(void) {
  if (!running) {
    return;
  }
  ps4_stage("usbio: stop");
  // Let queued messages (e.g. the 'D' before this) reach the M8.
  for (int i = 0; i < 60; i++) {
    SDL_LockMutex(tx_lock);
    int pending = tx_count;
    SDL_UnlockMutex(tx_lock);
    if (pending == 0 && !tx_busy) {
      break;
    }
    SDL_Delay(5);
  }
  stopping = 1;
  SDL_WaitThread(usb_thread, NULL);
  usb_thread = NULL;
  running = 0;
  if (dog_thread != NULL) {
    SDL_WaitThread(dog_thread, NULL);
    dog_thread = NULL;
  }
  // USB thread is gone: this thread owns sceUsbd again.
  free_transfers();
  release_audio_iface();
  ps4_stage("usbio: stopped");
}

bool usbio_prepare_audio(void) {
  if (audio_claimed) {
    return true;
  }
  load_overrides();
  return setup_audio_iface();
}

bool usbio_active(void) { return running != 0; }

int usbio_write(const uint8_t *buf, int len) {
  if (!running || len <= 0 || len > TX_MSG_MAX) {
    return -1;
  }
  SDL_LockMutex(tx_lock);
  if (tx_count == TX_QUEUE) {
    SDL_UnlockMutex(tx_lock);
    st.tx_drop++;
    return -1;
  }
  int slot = (tx_head + tx_count) % TX_QUEUE;
  tx_q[slot].len = (uint8_t)len;
  memcpy(tx_q[slot].b, buf, len);
  tx_count++;
  SDL_UnlockMutex(tx_lock);
  return len;
}

int usbio_read(uint8_t *buf, int count) {
  main_beat = SDL_GetTicks();
  if (dev_gone) {
    return -1;
  }
  SDL_LockMutex(rx_lock);
  uint32_t got = ring_buffer_pop(rx_ring, buf, count);
  SDL_UnlockMutex(rx_lock);
  return got == (uint32_t)-1 ? 0 : (int)got;
}

uint32_t usbio_audio_pop(uint8_t *buf, uint32_t len) {
  SDL_LockMutex(audio_lock);
  // Whole stereo frames only, so the consumer never loses alignment.
  uint32_t n = audio_ring->size < len ? audio_ring->size : len;
  n &= ~3u;
  uint32_t got = n ? ring_buffer_pop(audio_ring, buf, n) : 0;
  SDL_UnlockMutex(audio_lock);
  return got == (uint32_t)-1 ? 0 : got;
}

uint32_t usbio_audio_level(void) {
  SDL_LockMutex(audio_lock);
  uint32_t n = audio_ring->size;
  SDL_UnlockMutex(audio_lock);
  return n;
}

void usbio_note_underrun(void) { st.underruns++; }
