# m8c-ps5

[m8c](https://github.com/laamaa/m8c) (the Dirtywave M8 Headless client) running natively on a
jailbroken PS5. Plug the M8 into the console's USB port: the M8 screen shows on the TV, audio
plays through the TV and/or the DualSense speaker, and the DualSense drives the M8.

Ported from the PS4 port (`~/m8c-ps4`, build C7). Tested on PS5 firmware 11.60 with an M8
Headless (Teensy 4.1, firmware 6.5.2).

## Requirements

- PS5 on a jailbroken firmware (tested on 11.60: kstuff + elfldr).
- **websrv** running (the web UI / homebrew loader on port **8080**). It provides `/hbldr`.
- **ftpsrv** running (FTP on port **2121**), used for uploads, config and logs.
- An M8 Headless (or an M8 in headless mode) on a USB port of the PS5.

## Build (Linux host)

- ps5-payload-sdk at `~/ps5-jailbreak/refs/releases/sdk/ps5-payload-sdk`, built with LLVM 21.
- PacBrew homebrew sysroot (SDL2 2.30.12) unpacked at `downloads/opt/ps5-payload-sdk/`. It is
  gitignored, so fetch it again into `downloads/` on a fresh clone.

```sh
make            # -> m8c_ps5.elf
```

The Makefile sets `LLVM_CONFIG` to the SDK's llvm-config shim. Without it, the wrong clang
(theos clang-11 or system clang 22) gets picked up. See `notes/RECON.md` for toolchain details.

## Install and launch

```sh
make deploy                      # PS5_HOST defaults to 192.168.0.106
make deploy PS5_HOST=<ps5-ip>
```

`make deploy` uploads the ELF to `/data/homebrew/m8c/eboot.elf` over FTP. It then launches it with:

```
http://<ps5-ip>:8080/hbldr?pipe=0&daemon=0&path=/data/homebrew/m8c/eboot.elf
```

> **`daemon=0` is required.** A `daemon=1` launch gets no video memory, so the TV stays black
> while audio still plays. That instance also keeps running in the background and fights the next
> one over the M8.

Only one copy of m8c runs at a time. On startup, m8c kills any earlier copy listed in
`/data/m8c.pid`.

## Controls (DualSense)

| M8 key | DualSense |
|---|---|
| Arrow keys | D-pad (or left stick) |
| SHIFT | **Create** (or **L2**) |
| PLAY | **Options** (or **R2**) |
| OPTION | **Circle** |
| EDIT | **Cross** |
| Reset display | **L3** + Create/L2 |
| **Quit m8c** | **R3** + Create/L2 |

Triangle, Square, L1, R1 and the right stick are unmapped. Quitting returns you to the PS5 home
screen. The PS button only goes to the home screen and leaves m8c running.

### Remapping

Edit the `[gamepad]` section of `/data/m8c_config.ini` over FTP, then relaunch. Values are SDL
GameController button numbers:

| # | Button | # | Button |
|---|---|---|---|
| 0 | Cross | 8 | R3 |
| 1 | Circle | 9 | L1 |
| 2 | Square | 10 | R1 |
| 3 | Triangle | 11 | D-pad up |
| 4 | Create | 12 | D-pad down |
| 5 | PS | 13 | D-pad left |
| 6 | Options | 14 | D-pad right |
| 7 | L3 | | |

Analog axes (`gamepad_analog_axis_*`): 0 left X, 1 left Y, 2 right X, 3 right Y, 4 L2, 5 R2.
Use `-1` to disable an axis.

## Audio output

Set `audio_device_name` in the `[audio]` section of `/data/m8c_config.ini`:

| Value | Output |
|---|---|
| `Default` | TV / system output. This includes a headset on the DualSense jack, following the PS5 sound settings. |
| `speaker` | DualSense speaker only |
| `both` | TV and DualSense speaker |

The DualSense speaker only takes mono, so the M8's stereo is mixed down. Audio is captured from
the M8 over USB at 44.1 kHz and resampled to 48 kHz.

## Files on the console

| Path | What |
|---|---|
| `/data/homebrew/m8c/eboot.elf` | the app |
| `/data/m8c_config.ini` | m8c config (controls, audio, `wait_for_device`, ...) |
| `/data/m8c.log` | log: boot stages, USB/audio stats every 5 s, render fps |
| `/data/m8c.pid` | PID of the running copy (single-copy guard) |

Read the log with `curl ftp://<ps5-ip>:2121/data/m8c.log`.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Black screen, audio plays | Launched with `daemon=1`. Relaunch with `make deploy`. |
| Screen flickers, audio choppy | Two copies are sharing the M8, often a stray `daemon=1` one (shows up as `payload` in a process list). Relaunching kills it via the pidfile. Check the log for `SLIP error` lines and `iso` below ~176000 B/s. |
| Log says `Device not detected` | Unplug the M8 and plug it back in. m8c waits for it (`wait_for_device=true`). |
| Loader launches hang or return nothing | The system still has a dead app open. Press the PS button, then launch again. |
| UI feels slow | Rendering is about 15 fps (1080p software scaling + vsync), capped at 30. |

More detail on the port, its pitfalls and its history is in `notes/RECON.md`.

## Layout

- `app/src/`: upstream m8c v1.7.10 sources, with `PS4`/`PS5` guards.
- `port/`: PS5 backends. sceUsbd (loaded at runtime with dlopen) handles the USB serial link and
  UAC2 audio capture. sceAudioOut handles TV and pad-speaker output. Also logging, the single-copy
  guard and exit-to-home.
- `probe/`: the pre-port USB probe payload.
- `notes/RECON.md`: porting record.
