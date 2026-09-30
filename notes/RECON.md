# m8c-ps5 porting record (2026-09-30)

Native PS5 port of m8c (M8 headless client) as an elfldr payload, derived from
~/m8c-ps4 (C7 build). Console: PS5 11.60, kstuff/lapy-jb, elfldr on 9021,
Idlesauce Direct PKG/ELF Loader (ftpsrv FTP on 2121, web UI on 8080).

## RESULT (same day): USB CDC display + INPUT + UAC2 AUDIO ALL WORKING

Streaming stats from the first audio run:
- iso capture iface 3 alt 1, EP 0x85, 48B pkts: 176,366 B/s steady, 0 packet
  errors, 0 underruns, ring 4-8 KB, HandleEventsTimeout max 2 ms
- identical numbers to the PS4 C7 run. The C7 engine ports unchanged.

## Architecture

- app/src = the PS4 port's m8c v1.7.10 sources. Only 3 diffs vs PS4:
  main.c render.c config.c: `#ifdef PS4` guards widened to
  `#if defined(PS4) || defined(PS5)` (shims include, stage markers,
  /data config path) + a PS5 build-marker block in main.c.
- port/usbio_ps5.c = VERBATIM copy of the PS4 C7 engine except:
  include block (ps5_usbd_names.h + ps4_shims.h added) and the
  HandleEventsTimeout cast fixed ((int32_t*) -> (struct timeval*); OpenOrbis
  declared it wrong, the PS5 decl is the true libusb signature).
- port/usb_ps5.c = PS4 CDC backend with sceKernelLoadStartModule -> ps5_usbd_load().
- port/ps5_usbd.{c,h} + ps5_usbd_syms.inc + ps5_usbd_names.h = runtime
  dlopen("libSceUsbd.sprx") + dlsym table. NO PS5 stub lib exists for Usbd.
  Types come from the pacbrew sysroot's real <libusb.h> (same FreeBSD lineage
  as the console module - zero hand-rolled structs, unlike PS4).
- port/ps5_sceaudio.h + SDK stub libs = sceAudioOut (link -lSceAudioOut).
- port/ps5_shims.* = same /data/m8c.log channel as PS4.

## Pre-flight probe (probe/) — ran before any porting

dlopen/dlsym works from a jailed payload: all sceUsbd symbols resolve by
name; M8 enumerated, CDC handshake + fw read OK, UAC2 ifaces claim, alt
switch accepted, GetMaxIsoPacketSize(dev,0x85)=48. LESSON: the probe
crashed at GetMaxIsoPacketSize(devh,...) — PS5 takes device* (like PS4),
the wrong-type call kills the process. Probe teardown never ran; the M8
stayed in display mode until the app's init re-set it.

## Build (Manjaro)

- SDK: ~/ps5-jailbreak/refs/releases/sdk/ps5-payload-sdk (pinned release).
- llvm-config shim REQUIRED before any compile: wrappers resolve clang/ld.lld
  via `llvm-config --bindir`; Manjaro has no /usr/lib/llvm21/bin/ld.lld
  (tools live in /usr/bin) and theos clang-11 would otherwise win.
  export LLVM_CONFIG=~/ps5-jailbreak/refs/releases/sdk/llvm-config-shim.sh
- The console SDK was built with LLVM 21; system clang 22 emits its sprx
  imports wrong. Use the shim (routes to llvm21 tooling).
- prospero-clang auto-adds --sysroot and -L target/lib + target/user/
  homebrew/lib, but SDL2 HEADERS need explicit -I<sysroot>/include/SDL2 and
  the pacbrew libs need -L<pacbrew>/lib (SDK's own homebrew/lib is empty).
- Pacbrew sysroot: ~/m8c-ps5/downloads/opt/ps5-payload-sdk/target/user/
  homebrew (SDL2 2.30.12 static + iconv + stubs' deps).
- LDLIBS: -lSDL2 -lSceAudioOut -lSceUserService -lSceVideoOut -lSceKeyboard
  -lSceImeDialog -lSceSystemService -lScePad -liconv
- prospero-deploy needs socat (not installed) -> use
  ~/ps5-jailbreak/scripts/send_payload.py instead.

## Launching on the console (the non-obvious part)

- elfldr 9021 accepts ONE payload per session. m8c never exits -> every
  later send silently no-ops (bytes accepted, never run, log untouched).
- The old instance can go ZOMBIE after device loss (stuck in SDL_Quit
  releasing video it never owned). Nothing on 9021/8080 kills it.
- The web UI on 8080 (Idlesauce Direct PKG/ELF Loader, behind pldmgr) has:
    GET /hbldr?pipe=0|1&daemon=0|1&path=/data/x.elf[&args=...]
  pipe=1 streams the app's stdout (use --max-time! closing the pipe kills
  the app); daemon=1 survives the HTTP request. Upload the ELF over FTP
  (2121) to /data first. This is the only launch path that works once
  9021 is wedged.
- Init order quirk: m8c reads+ADOPTS its config BEFORE ps4_log_init opens
  /data/m8c.log. If the app dies in read_config, the log file never
  truncates and looks stale. pipe=1 stdout is the early-boot channel.
- A jailed payload has NO app icon and NO TV output ("bigapp" launch
  context needed for that later). SDL still works offscreen: both
  "sceVideoOutOpen: Device busy" and "sceKernelAllocateMainDirectMemory"
  warnings are survived by the software-renderer path.

## Config lessons

- SDL_GetPrefPath WORKS on PS5 SDL (/user/home/<id>/m8c/) and the generic
  config.c path therefore silently used it on the first run - the PS4/PS5
  /data override must be compiled in (fixed via the widened guard).
- Ship the FULL 932-byte config (copied from the PS4's /data), not a 2-line
  override: wait_for_device=true matters for headless wait-for-plug.

## Runtime findings

- First boot inits usbd lazily; after a clean shutdown a second init works
  (module handle changes) - no need to reload.
- Device loss (unplug) is detected and handled exactly like PS4
  (0x802400FF on bulk IN -> shutdown; the final 'D' send may fail with
  0x80240004 if the device is already gone - harmless).
- WATCHDOG "usb thread stuck 4294967295ms" twice early = cosmetic underflow
  in the watchdog's tick math (same race as PS4); self-clears, stats keep
  flowing every 5 s.
- Controllers enumerate ("PS5 Remote Control" etc.) but names unmapped;
  input mapping untested (SDL 2.30.12 - PS4's hand-written DS4 mapping
  dance should NOT be needed, but verify buttons in-app).

## TV output fix (2026-09-30)

Black screen had two causes:
1. hbldr `daemon=1` launches get NO direct-memory budget: SDL's ps5 video
   driver allocates a fixed 64 MiB (sceKernelAllocateMainDirectMemory) and
   gets EAGAIN. Launch with `daemon=0` (`make deploy`). Proved with a
   standalone probe: same alloc succeeds under daemon=0.
2. PS5 build took the desktop SDL_INIT_EVERYTHING + OpenGL branch in
   render.c; now shares the PS4 window-surface + software-renderer path.
Each hbldr launch replaces the previous app (one bigapp at a time).

## Open items

1. TV display mirror: needs launch as "bigapp" (Homebrew Loader PKG context)
   - headless payloads render offscreen only.
2. Verify DS/DualSense button input through SDL 2.30.12.
3. Fix the watchdog underflow (harmless but noisy).
4. Audio is left-channel-only on PS4 headset; same check needed on PS5.
