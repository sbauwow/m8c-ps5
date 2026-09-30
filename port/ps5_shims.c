// PS5 implementation of the ps4_shims contract (see ps5_shims.h).
// Console-agnostic except for the log path, which is the same /data/m8c.log
// the PS4 port uses — ftpsrv exposes /data over FTP on 2121, same as GoldHEN.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "ps5_shims.h"

static FILE *g_logfile = NULL;

static void ps5_log_output(void *userdata, int category, SDL_LogPriority priority,
                           const char *message) {
  (void)userdata;
  (void)category;
  (void)priority;
  if (g_logfile) {
    fprintf(g_logfile, "%s\n", message);
    fflush(g_logfile);
  }
}

static void log_open_locked(void) {
  if (g_logfile == NULL) {
    g_logfile = fopen("/data/m8c.log", "w");
    if (g_logfile) {
      fprintf(g_logfile, "=== m8c PS5 ===\n");
    }
  }
}

void ps5_log_init(void) {
  log_open_locked();
  if (g_logfile) {
    fflush(g_logfile);
  }
  SDL_LogSetOutputFunction(ps5_log_output, NULL);
}

void ps5_stage(const char *msg) {
  log_open_locked();
  if (g_logfile) {
    fprintf(g_logfile, "[stage] %s\n", msg);
    fflush(g_logfile);
  }
}

void ps5_stage_once(const char *msg) {
  static const char *seen[64];
  static int seen_count = 0;
  for (int i = 0; i < seen_count; i++) {
    if (seen[i] == msg) {
      return; // string literals: pointer identity is enough
    }
  }
  if (seen_count < 64) {
    seen[seen_count++] = msg;
  }
  ps5_stage(msg);
}

void ps5_logf(const char *fmt, ...) {
  log_open_locked();
  if (g_logfile) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logfile, fmt, ap);
    fprintf(g_logfile, "\n");
    fflush(g_logfile);
    va_end(ap);
  }
}

const char *ps5_pref_path(const char *filename) {
  static char path[512];
  snprintf(path, sizeof(path), "/data/m8c_%s", filename);
  return path;
}
