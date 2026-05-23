# NapBuster ⌚

A Pebble smartwatch app that stops you from napping during the day so you can fall asleep easier at night.

When it detects you've dozed off during your configured no-nap hours, it vibrates until you wake up and dismiss it.

---

## How it works

NapBuster runs a **background worker** that listens to Pebble's built-in sleep detection (HealthService). When it sees you've fallen asleep during your no-nap window, it launches the foreground app which vibrates repeatedly until you dismiss it.

```
Background worker (always running)
    │
    ├─ HealthService detects sleep
    ├─ Is it a no-nap day + hour? ──No──▶ ignore
    └─ Yes ──▶ launch foreground app
                    │
                    ├─ Red screen + repeating vibration
                    ├─ SELECT ──▶ dismiss
                    ├─ UP     ──▶ snooze 10 min
                    └─ DOWN   ──▶ snooze 30 min
```

**Battery efficient:** the worker only subscribes to HealthService during your active hours. Outside those hours it idles with just a 60-second clock check — no sensors running.

---

## Features

- 🔍 **Automatic sleep detection** via Pebble HealthService (no manual trigger needed)
- 📳 **Repeating vibration alarm** — keeps buzzing until dismissed
- 💤 **Snooze** — 10 or 30 minutes, re-arms automatically via Wakeup API (survives app close)
- 📅 **Per-day schedule** — pick exactly which days to guard (like a standard alarm clock)
- 🕐 **Configurable no-nap hours** — set your own start and end time
- 💪 **Vibration strength** — Gentle / Medium / Strong
- 🔋 **Battery efficient** — HealthService only active during guarded hours

---

## Settings

Open settings from the main screen with a **long-press on SELECT**.

| Setting | Description | Default |
|---|---|---|
| **Guard** | Master on/off switch | ON |
| **Active days** | Which days of the week to guard | Every day |
| **No-nap from** | Start of the no-nap window | 11:00 |
| **No-nap until** | End of the no-nap window | 23:00 |
| **Wake vibration** | Alarm intensity | Medium |

### Active days picker

Pressing SELECT on **Active days** opens a day-by-day picker (just like the built-in alarm clock):

- **UP / DOWN** — navigate days
- **SELECT** — toggle a day on/off (✓ = active)
- **BACK** — save and return

The settings row shows a smart summary: `Every day`, `Weekdays`, `Weekends`, or `N days`.

### Vibration modes

| Mode | Pattern |
|---|---|
| **Gentle** | 2 slow soft pulses, long rest |
| **Medium** | Escalating short → long bursts |
| **Strong** | Rapid-fire dense buzzing |

*Pebble has no hardware intensity control — modes are simulated via pulse density.*

---

## Controls

### Main screen
| Button | Action |
|---|---|
| SELECT (long press) | Open settings |

### Alarm screen (when nap detected)
| Button | Action |
|---|---|
| SELECT | Dismiss alarm |
| UP | Snooze 10 minutes |
| DOWN | Snooze 30 minutes |

---

## Supported watches

| Platform | Watch |
|---|---|
| **emery** | Pebble Time 2 *(primary target)* |
| **basalt** | Pebble Time, Pebble Time Steel |
| **chalk** | Pebble Time Round |
| **diorite** | Pebble 2 |

> Requires Pebble Health to be enabled in the rePebble mobile app (Devices → Health).

---

## Building & installing

### Prerequisites

```bash
# Install pebble-tool (requires Python 3.11)
uv venv ~/.pebble-venv --python 3.11
~/.pebble-venv/bin/pip install pebble-tool
~/.pebble-venv/bin/pebble sdk install latest
```

> See [developer.repebble.com/sdk](https://developer.repebble.com/sdk/) for full SDK setup instructions.

### Build

```bash
cd nap-buster
~/.pebble-venv/bin/pebble build
```

Output: `build/napbuster.pbw`

### Install to emulator

```bash
~/.pebble-venv/bin/pebble install --emulator emery
```

### Install to your watch

Enable **Dev Connect** in the rePebble mobile app (Devices → tap your watch → ⋯ → Enable Dev Connect → sign in with GitHub), then:

```bash
~/.pebble-venv/bin/pebble login
~/.pebble-venv/bin/pebble install --cloudpebble
```

---

## Project structure

```
nap-buster/
├── package.json              # App config, capabilities, target platforms
├── wscript                   # Waf build script
├── src/c/
│   ├── main.c                # Foreground app — alarm UI, snooze, launch handling
│   ├── settings.c            # Settings screen — 5 rows with virtual scroll
│   ├── settings.h
│   ├── days_window.c         # Day picker sub-screen (per-day schedule)
│   ├── days_window.h
│   └── common.h              # Shared constants, persist keys, vibe patterns, helpers
└── worker_src/c/
    └── worker.c              # Background worker — sleep monitoring, window logic
```

### Key design decisions

**`common.h` is the single source of truth** for all persist keys, default values, vibration patterns, and the `is_in_no_nap_window()` helper. Both the foreground app and worker inline what they need from it.

**The worker never polls.** It subscribes to `health_service_events_subscribe()` which fires only on OS-detected state changes. Outside the no-nap window the subscription is dropped entirely.

**Snooze survives app close** via `wakeup_schedule()`. The OS wakes the app at the right time even if it was killed.

**Active days** are stored as a bitmask (`uint8_t`): bit 0 = Sunday, bit 6 = Saturday. Preset values: `0x7F` = every day, `0x3E` = weekdays, `0x41` = weekends.

---

## Persist key reference

| Key | Value | Notes |
|---|---|---|
| `0` | `ENABLED` | bool — master on/off |
| `1` | `START_HOUR` | int 0–23 |
| `2` | `END_HOUR` | int 0–23 |
| `3` | `SNOOZE_UNTIL` | time_t epoch, 0 = none |
| `4` | `ALARMING` | bool — alarm currently active |
| `5` | `WAKEUP_ID_SNOOZE` | WakeupId for snooze re-arm |
| `7` | `VIBE_STRENGTH` | 0=Gentle 1=Medium 2=Strong |
| `8` | `ACTIVE_DAYS` | uint8 bitmask (bit0=Sun..bit6=Sat) |

---

## Memory usage (emery / Pebble Time 2)

| Component | RAM used | Budget |
|---|---|---|
| Background worker | ~1.8 KB | 10.5 KB |
| Foreground app | ~5.1 KB | 128 KB |

---

## A note on sleep detection latency

Pebble's HealthService confirms sleep after approximately **5–10 minutes** of inactivity and low movement — not the instant you close your eyes. NapBuster will buzz you shortly into a nap rather than the very second you doze off, which is still effective for the use case.

---

## License

MIT
