// ps5_usbd.c — dlopen/dlsym binding of libSceUsbd.sprx (see ps5_usbd.h).
//
// The definition and dlsym passes below run under the SAME name mapping as
// every consumer (ps5_usbd_names.h): they expand to p_-prefixed identifiers,
// so the pointers defined here are exactly the ones the engine calls. The
// dlsym lookup strings come from #name (the macro argument token), which the
// mapping does not touch — they stay "sceUsbdInit" etc.

#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#include "ps5_shims.h"

// Order matters: the name mapping first, so ps5_usbd.h's extern pass (and
// the passes below) expand to the p_-prefixed identifiers via macro rescanning.
#include "ps5_usbd_names.h"
#include "ps5_usbd.h"

// Define the function pointers (expands to p_sceUsbd* definitions).
#define USBD_DECL(ret, name, params) name##_fn name;
#include "ps5_usbd_syms.inc"
#undef USBD_DECL

static void *g_mod = NULL;
static int g_ready = 0;

int ps5_usbd_ready(void) { return g_ready; }

int ps5_usbd_load(void) {
  if (g_ready) {
    return 0;
  }
  if (g_mod == NULL) {
    g_mod = dlopen("libSceUsbd.sprx", RTLD_LAZY);
    if (g_mod == NULL) {
      const char *e = dlerror();
      ps5_logf("usbd: dlopen failed: %s", e ? e : "(none)");
      return -1;
    }
    ps5_logf("usbd: module handle %p", g_mod);
  }

  int missing = 0;
#define USBD_DECL(ret, name, params)                                           \
  name = (name##_fn)dlsym(g_mod, #name);                                       \
  if (name == NULL) {                                                          \
    ps5_logf("usbd: MISSING %s", #name);                                       \
    missing++;                                                                 \
  }
#include "ps5_usbd_syms.inc"
#undef USBD_DECL

  if (missing > 0) {
    ps5_logf("usbd: %d symbol(s) missing, load failed", missing);
    return -1;
  }
  ps5_logf("usbd: all symbols resolved");

  if (sceUsbdInit() < 0) {
    ps5_logf("usbd: sceUsbdInit failed");
    return -1;
  }
  g_ready = 1;
  ps5_logf("usbd: ready");
  return 0;
}
