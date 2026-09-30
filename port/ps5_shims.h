// PS5 logging/diagnostics shims (same contract as the PS4 ps4_shims).
//
// A jailed PS5 payload's stdout goes nowhere, so every SDL_Log line is
// mirrored to /data/m8c.log (readable over ftpsrv FTP, port 2121). SDL log
// redirection is unreliable across SDL builds, so the stage/logf helpers
// write the file directly. All paths are flat in /data — Fios2-style /data
// access was proven by the phase-0 probe (dlopen'd module wrote its log).
#ifndef PS5_SHIMS_H_
#define PS5_SHIMS_H_

// Mirror all SDL_Log output to /data/m8c.log. Call before anything else logs.
void ps5_log_init(void);

// Raw stage marker, independent of SDL_Log: writes "[stage] msg" to the log.
// Used to bisect boot hangs (a dead payload gives no diagnostics).
void ps5_stage(const char *msg);

// Like ps5_stage, but each unique message is logged only the first time it is
// seen — safe to call every frame.
void ps5_stage_once(const char *msg);

// printf-style line directly to /data/m8c.log.
void ps5_logf(const char *fmt, ...);

// Replaces SDL_GetPrefPath("", file) on PS5: flat /data/m8c_<file>.
const char *ps5_pref_path(const char *filename);

#endif
