// PS5 implementation of the ps4_shims contract (see ps5_shims.h).
// Console-agnostic except for the log path, which is the same /data/m8c.log
// the PS4 port uses — ftpsrv exposes /data over FTP on 2121, same as GoldHEN.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

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

#define PIDFILE "/data/m8c.pid"

// hbldr names its processes "payload" (daemon=1) or after the ELF file.
static int looks_like_m8c(pid_t pid) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
  struct kinfo_proc ki;
  size_t len = sizeof(ki);
  if (sysctl(mib, 4, &ki, &len, NULL, 0) != 0 || len == 0) {
    return 0; // gone
  }
  return strcmp(ki.ki_comm, "payload") == 0 || strcmp(ki.ki_comm, "eboot.elf") == 0 ||
         strncmp(ki.ki_comm, "m8c", 3) == 0;
}

int ps5_kill_previous_instance(void) {
  int killed = 0;
  FILE *f = fopen(PIDFILE, "r");
  if (f) {
    int old = 0;
    if (fscanf(f, "%d", &old) == 1 && old > 1 && old != getpid() && looks_like_m8c(old) &&
        kill(old, SIGKILL) == 0) {
      killed = old;
      usleep(500 * 1000); // let the kernel release its USB interfaces
    }
    fclose(f);
  }
  f = fopen(PIDFILE, "w");
  if (f) {
    fprintf(f, "%d\n", getpid());
    fclose(f);
  }
  return killed;
}

int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int opt, int method, int reason);

void ps5_exit_to_home(void) {
  const int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
  ps5_logf("exit: killing app 0x%x", app_id);
  if (app_id > 0) {
    const int rc = sceSystemServiceKillApp(app_id, -1, 0, 0);
    ps5_logf("exit: KillApp -> 0x%08X", rc);
  }
  unlink("/data/m8c.pid");
}

typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int blocking);

void ps5_notify(const char *text) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  strncpy(req.message, text, sizeof(req.message) - 1);
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}
