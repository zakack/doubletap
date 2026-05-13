# osu-intercept — Keyboard SOCD Filter Daemon

A standalone Linux daemon that applies SOCD (Simultaneous Opposite Cardinal Direction) cleaning to keyboard input with real-time audio feedback. Designed for rhythm games like osu!

## What It Does

- Grabs a physical keyboard device via evdev
- Tracks the last two pressed keys with LRU ordering
- Emits two mutually-exclusive virtual keys (radio-button toggle behavior)
- Plays a WAV click sound on every virtual key press via PipeWire
- All other keys pass through normally to a virtual uinput device
- Drops root privileges after device setup

## How It Works

```
Physical Keyboard → [evdev grab] → SOCD State Machine → [uinput virtual device] → Applications
                                              ↓
                                    PipeWire Audio Click
```

- Press a key → virtual key 1 pressed, click plays
- Press a different key while first held → virtual key 1 released, virtual key 2 pressed, click plays
- Release a key while the other is held → virtual key toggles back, click plays
- Release the last key → virtual key released, state resets

This promotes a "rocking" or "wobbling" play pattern when fast key alternation is needed without interfering with single-key tapping. Particularly useful for osu! and other rhythm games with odd-numbered bursts and streams.

## Requirements

- Linux kernel with uinput support (`CONFIG_INPUT_UINPUT`)
- libevdev, libpipewire-0.3, libyaml
- CMake and a C99 compiler
- Root access (for evdev grab and uinput device creation)

## Installation

```bash
cmake -B build && cmake --build build
sudo cmake --install build
```

## Configuration

Edit `/etc/osu-intercept/config.yaml`:

```yaml
device: "/dev/input/by-id/usb-Wooting_60HE-event-kbd"  # Your keyboard
audio:
  wav_path: "/usr/local/share/osu-intercept/click.wav"
mapping:
  trigger_keys: []          # All keys trigger SOCD; or [30, 48] for specific keys
  virtual_keys: [183, 184]  # KEY_F13, KEY_F14
```

## Finding Your Keyboard

```bash
ls /dev/input/by-id/              # List all input devices by ID
sudo evtest                       # Interactive device tester — press keys to identify
```

## Service Management

```bash
sudo systemctl enable --now osu-intercept
sudo systemctl status osu-intercept
sudo journalctl -u osu-intercept  # View logs
```

## Troubleshooting

- **"Cannot access device"**: Check device path in config. Try `sudo evtest` to verify.
- **"uinput create failed"**: Ensure `uinput` kernel module is loaded: `sudo modprobe uinput`
- **No audio**: Ensure PipeWire is running: `systemctl --user status pipewire`
- **Permission denied**: The daemon must run as root initially (drops to nobody after setup)
