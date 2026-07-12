# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`osu-interceptd` is a single-file C11 daemon that acts as a SOCD (Simultaneous
Opposing Cardinal Directions) cleaner for two keyboard keys. It exclusively
grabs one or more evdev keyboard devices at the kernel level (before
Xorg/Wayland see them), applies a "last-input + reverting toggle" state
machine to two configured physical keys, and re-emits all input through a
single uinput virtual keyboard. It also plays a click sound via PipeWire on
every virtual keypress. Built for rhythm games (osu!) where fast key
alternation needs a "rocking" input pattern instead of naive SOCD handling.

The entire implementation lives in `osu-interceptd.c` (~980 lines) — there is
no multi-module structure.

## Build

```sh
cmake -S . -B build
cmake --build build
```

Produces `build/osu-interceptd`. Requires pkg-config-visible dev packages for
`libpipewire-0.3`, `libevdev`, and `yaml-0.1` (libyaml).

There is no test suite, linter, or CI config in this repo — validate changes
by building and manually exercising the daemon (see Running below).

## Running

The daemon needs read access to `/dev/input/event*` and read/write access to
`/dev/uinput`, so it's normally run as root (or a user in the `input` group
with a udev rule granting uinput access — see `osu-intercept.service`).

```sh
sudo ./build/osu-interceptd -c config.yaml
```

`-c` points to a YAML config (default `/usr/share/osu-intercept/config.yaml`).
See `config.yaml` for the schema: `devices` (required list of
`/dev/input/by-id/*` paths), `keys` (k1/k2 physical -> v1/v2 virtual, symbolic
`KEY_*` names or numeric codes), `audio` (enabled + wav path), `uinput`
(virtual device name). After editing config, restart via
`sudo systemctl restart osu-intercept` when installed as the systemd unit in
`osu-intercept.service`.

## Architecture

Everything is in `osu-interceptd.c`, organized into clearly delimited
sections (search for the `/* --- */` banner comments):

1. **Config** (`load_config` and friends) — parses YAML via libyaml's
   document API (not the streaming API) into an `oid_config_t`. Key codes
   accept either symbolic names (resolved via
   `libevdev_event_code_from_name`) or raw integers.

2. **Input devices** (`input_open`/`input_close`) — each configured device
   path is opened, wrapped in a `libevdev` handle, and exclusively grabbed
   (`LIBEVDEV_GRAB`) so events don't leak to the rest of the system.

3. **Virtual uinput device** (`build_virtual`) — a single synthetic keyboard
   is created that unions every key code found on all grabbed source
   devices, plus the two configured virtual keys (v1/v2). All non-k1/k2
   events are mirrored through verbatim.

4. **Radio-button state machine** (`process_event`) — the core logic. Each
   input device tracks its own `k1`/`k2`/`act` (which virtual key is
   currently "on") state independently. The transition is computed as
   `state = k1 + k2 + event.value` after updating k1/k2, giving four cases
   (`S_NONE`, `S_RELEASE`, `S_SINGLE`, `S_PRESS`) that decide whether to
   emit a virtual key-down, a release, or an release-then-press pair (the
   "reverting toggle" that recreates a keypress when the active key is
   released while the other is still held). Autorepeat (`value == 2`) is
   ignored to avoid corrupting state.

5. **Event loop** (`run_loop`/`drain_device`) — a single-threaded
   `epoll`-based loop multiplexes all grabbed devices, draining each with
   `libevdev_next_event` (handling `LIBEVDEV_READ_STATUS_SYNC` dropped-event
   resync) until `EAGAIN`. A device is dropped from the poll set on
   error/HUP without killing the daemon, provided at least one device
   remains.

6. **Audio** (`wav_load`, `audio_init`, `on_process`, `audio_trigger`) — a
   hand-rolled WAV reader (16/24/32-bit PCM) loads the click sample into a
   float buffer up front. Playback runs on a dedicated PipeWire thread loop
   (`pw_thread_loop`); `on_process` is the realtime audio callback and only
   touches shared state through `atomic_*` operations (`playing`, `pending`,
   `reset`, `frame_pos`) since it runs on PipeWire's RT thread while
   `audio_trigger` is called from the main epoll thread. Overlapping
   triggers restart playback from frame 0 rather than queuing.

`main()` wires these together: parse args -> load config -> attempt
`SCHED_FIFO` realtime priority (best-effort, warns and falls back on
failure) -> best-effort audio init (`mlockall` if audio is enabled, to avoid
page faults in the RT callback) -> open/grab all configured devices (aborts
only if *none* could be opened) -> build the virtual device -> install
SIGINT/SIGTERM handlers -> run the event loop -> tear down in reverse order.

## Key invariants to preserve when editing `process_event`

- `k1`/`k2`/`act` state is per-input-device (`input_dev_t`), not global —
  multiple physical keyboards are tracked independently even though they
  share one virtual output device.
- Every emitted `EV_KEY` write must be followed by an `EV_SYN`/`SYN_REPORT`
  before the next state-changing write, otherwise userspace sees coalesced
  events.
- `MSC_SCAN` events are dropped rather than mirrored; all other non-k1/k2
  events pass through untouched.
