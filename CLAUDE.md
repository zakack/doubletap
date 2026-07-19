# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`doubletapd` is a single-file C11 daemon that acts as a SOCD (Simultaneous
Opposing Cardinal Directions) cleaner for two keyboard keys. It exclusively
grabs evdev keyboard devices at the kernel level (before Xorg/Wayland see
them) — either an explicit config list or, by default, every
keyboard-shaped device advertising both configured keys, with
inotify-driven hotplug in both cases — applies a SOCD state machine to two
configured physical keys — "toggle" (last-input + reverting toggle; "on"
is a synonym), "snappy" (last input wins, no latching), or "off" (no
cleaning, just the k1/k2 -> v1/v2 remap), selected by the top-level
`socd` config field — and re-emits all input through a
single uinput virtual keyboard. It also plays a click sound via PipeWire on
every virtual keypress. Built for rhythm games (osu!) where fast key
alternation needs a "rocking" input pattern instead of naive SOCD handling.

The entire implementation lives in `doubletapd.c` (~980 lines) — there is
no multi-module structure.

## Build

```sh
cmake -S . -B build
cmake --build build
```

Produces `build/doubletapd`. Requires pkg-config-visible dev packages for
`libpipewire-0.3`, `libevdev`, and `yaml-0.1` (libyaml).

There is no test suite, linter, or CI config in this repo — validate changes
by building and manually exercising the daemon (see Running below).

## Running

The daemon is designed to run as an unprivileged user in the `input` group:
that covers read access to `/dev/input/event*`, and the packaged udev rule
(`70-doubletap-uinput.rules`) opens `/dev/uinput` to the same group. It
must NOT run as a root system service — PipeWire is a per-user session
daemon, so audio only works from inside a user session. The systemd unit
(`doubletap.service.in`, configured by CMake) is a *user* unit:
`systemctl --user enable --now doubletap`.

```sh
./build/doubletapd -c config.yaml
```

Without `-c`, the daemon looks for
`$XDG_CONFIG_HOME/doubletap/config.yaml` (`~/.config` if unset), then
falls back to the installed default (`/usr/share/doubletap/config.yaml`;
CMake bakes the real prefix in via `DEF_CONFIG`/`DEF_WAV` compile
definitions). See `config.yaml` for the schema: `devices` (optional list of
`/dev/input/by-id/*` paths; omitted or `auto` means auto-discovery), `socd`
(`toggle` default, `on` as a synonym, `snappy`, or `off`), `keys`
(k1/k2 physical -> v1/v2 virtual, symbolic `KEY_*` names or numeric codes),
`audio` (enabled + wav path), `uinput`
(virtual device name). After editing config, restart via
`systemctl --user restart doubletap`. The `-i DIR` flag overrides the
scanned/watched device directory (default `/dev/input`) — mainly for
integration testing against a directory of symlinks to synthetic uinput
nodes.

`packaging/arch/` holds an AUR-style `-git` PKGBUILD.

## Architecture

Everything is in `doubletapd.c`, organized into clearly delimited
sections (search for the `/* --- */` banner comments):

1. **Config** (`load_config` and friends) — parses YAML via libyaml's
   document API (not the streaming API) into an `oid_config_t`. Key codes
   accept either symbolic names (resolved via
   `libevdev_event_code_from_name`) or raw integers.

2. **Input devices** (`input_try_open`/`input_close`/`reconcile_devices`) —
   devices are opened, wrapped in `libevdev` handles, and exclusively
   grabbed (`LIBEVDEV_GRAB`) so events don't leak to the rest of the
   system. `reconcile_devices` is the single (re)open path, run at startup
   and again on every inotify event under the input dir: in explicit mode
   it retries any configured path not currently open; in auto-discovery
   mode it scans `event*` nodes and grabs those passing `auto_grab_ok`
   (has both k1 and k2, no `EV_REL`/`EV_ABS`, not `BUS_VIRTUAL`). In BOTH
   modes `is_doubletap_output` refuses any device whose uniq is
   `"doubletap"` or whose name matches the configured uinput name —
   grabbing our own output would be an instant feedback loop. If any key
   is physically down at open time (`any_key_down` — e.g. the Enter that
   launched the daemon), the grab is *deferred*: the fd stays open
   ungrabbed (`in->grabbed == 0`), `drain_device` discards its events
   (the system still receives them directly), and the grab completes as
   soon as the device reports all keys up — grabbing mid-press would
   swallow the release and leave the raw key stuck. Open devices
   are deduped by `st_rdev` (a node reached via two symlinks is grabbed
   once) and tracked in a `dev_list_t` of stable heap pointers (epoll user
   data points at the entries).

3. **Virtual uinput device** (`build_virtual`) — a single synthetic
   keyboard is created with a fixed keyboard-wide key set (every `KEY_*`
   code, skipping the `BTN_*` pointer/gamepad ranges) plus the two
   configured virtual keys (v1/v2). The set is fixed rather than a union
   of source devices because uinput capabilities are immutable after
   creation and hotplugged keyboards may carry codes the startup set
   lacked. All non-k1/k2 events are mirrored through verbatim.

4. **Radio-button state machine** (`process_event`) — the core logic. Each
   input device tracks its own `k1`/`k2`/`act` (which virtual key is
   currently "on") state independently. The transition is computed as
   `state = k1 + k2 + event.value` after updating k1/k2, giving four cases
   (`S_NONE`, `S_RELEASE`, `S_SINGLE`, `S_PRESS`) that decide whether to
   emit a virtual key-down, a release, or an release-then-press pair (the
   "reverting toggle" that recreates a keypress when the active key is
   released while the other is still held). The `S_RELEASE` case is
   mode-dependent: `SOCD_TOGGLE` reverts no matter which of the two held
   keys was released (latching), while `SOCD_SNAPPY` ("last input wins")
   only reverts when the *active* key is released — releasing the
   already-suppressed key emits nothing. Autorepeat (`value == 2`) is
   ignored to avoid corrupting state. `SOCD_OFF` bypasses the state
   machine before any of this: k1/k2 are remapped one-to-one to v1/v2
   (autorepeat included), presses still return 1 for the audio trigger,
   and `k1`/`k2` are still tracked so `release_stuck` works.

5. **Event loop** (`run_loop`/`drain_device`) — a single-threaded
   `epoll`-based loop multiplexes all grabbed devices plus an inotify fd
   watching the input dir (and its `by-id`/`by-path` subdirs) with
   `IN_CREATE | IN_ATTRIB | IN_MOVED_TO`; `IN_ATTRIB` matters because a
   hotplugged node is typically root-only until udev applies the
   input-group permissions, so the first open gets `EACCES` and the chmod
   retriggers the reconcile pass. Devices are drained with
   `libevdev_next_event` (handling `LIBEVDEV_READ_STATUS_SYNC`
   dropped-event resync) until `EAGAIN`. A device is dropped from the poll
   set on error/HUP without killing the daemon — releasing its active
   virtual key first if one was held (`release_stuck`) — and the loop
   keeps running with zero devices, waiting for hotplug (it only aborts at
   startup if nothing opened AND inotify is unavailable).

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
failure) -> best-effort audio init (`mlockall` if audio is enabled, to
avoid page faults in the RT callback) -> build the virtual device (safe
before any grab: discovery skips it via `is_doubletap_output`) -> install
SIGINT/SIGTERM handlers -> run the event loop (which opens/grabs devices
via the initial reconcile pass and handles hotplug thereafter) -> tear
down in reverse order.

## Key invariants to preserve when editing `process_event`

- `k1`/`k2`/`act` state is per-input-device (`input_dev_t`), not global —
  multiple physical keyboards are tracked independently even though they
  share one virtual output device.
- Every emitted `EV_KEY` write must be followed by an `EV_SYN`/`SYN_REPORT`
  before the next state-changing write, otherwise userspace sees coalesced
  events.
- `MSC_SCAN` events are dropped rather than mirrored; all other non-k1/k2
  events pass through untouched.
- The daemon must never grab a doubletap output device
  (`is_doubletap_output`, keyed on uinput uniq `"doubletap"` and the
  configured device name) — every path that opens an input device has to
  keep going through this guard, in explicit mode too, or emitted events
  feed straight back in as input (infinite feedback loop).
