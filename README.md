# NapBuster ⌚

**v1.1.0** — A Pebble smartwatch app that stops you from napping during the day so you can fall asleep easier at night.

When it detects you're falling asleep during your configured no-nap hours, it vibrates until you wake up and dismiss it.

---

## How it works

NapBuster runs a **background worker** with two-tier sleep detection:

```
Background worker (always running)
    │
    ├─ TIER 1: Piggybacks on OS heart rate events (Pebble Time 2 / Pebble 2)
    │       OS takes HR sample → HealthEventHeartRateUpdate fires for free
    │           └──▶ read HR + accel, run analysis immediately
    │       5-min fallback timer (only fires if no HR event arrived recently)
    │           └──▶ peek HR ourselves (handles slow/off background sampling)
    │       Two consecutive detections of HR drop + stillness? ──▶ alarm (~10–15 min)
    │
    ├─ TIER 2: Pebble HealthService sleep confirmation (all platforms, fallback)
    │       OS confirms sleep ──▶ alarm (45–90 min latency, but reliable)
    │
    └─ Alarm fires ──▶ launch foreground app
                           │
                           ├─ Repeating vibration
                           ├─ SELECT ──▶ dismiss
                           ├─ UP     ──▶ snooze 10 min
                           └─ DOWN   ──▶ snooze 30 min
```

**Battery efficient:** Tier 1 piggybacks on HR samples the OS was already taking — no extra CPU wakes. The 5-min fallback timer almost never does real work on the default 10-min sampling setting. All sensors are idle outside your guard window.

---

## Version history

To check which version you have installed: open NapBuster → long-press SELECT → the version is shown at the bottom of the settings screen. Or check the Pebble app under App Settings → About.

| Version | What changed |
|---|---|
| **1.4.0** | Debug telemetry on GUARDING screen (`HR:68 avg:74 d:142 x1`); Detection sensitivity setting (Sensitive/Balanced/Conservative) |
| **1.3.0** | Alarm screen has per-button labels (UP/SEL/DN) instead of cramped single hint; Tier 1 HR buffer now stays warm outside the guard window so detection can fire immediately at window open |
| **1.2.0** | Tier 1 now piggybacks on OS HR events (zero extra battery); 5-min timer only fires if no HR event arrived recently (handles slow/disabled background sampling) |
| **1.1.0** | Two-tier detection: HR+accel early warning on Time 2/Pebble 2; HealthService sleep event as fallback on all platforms |
| 1.0.0 | Initial release — HealthService sleep event detection only |

---

## Features

- 🔬 **Early nap detection** — piggybacks on OS HR events + accel to catch nap onset in ~10–15 min (Pebble Time 2 / Pebble 2)
- 🛡️ **Fallback detection** — Pebble's native sleep confirmation as a safety net on all platforms
- 📳 **Repeating vibration alarm** — keeps buzzing until dismissed
- 💤 **Snooze** — 10 or 30 minutes, re-arms automatically via Wakeup API (survives app close)
- 📅 **Per-day schedule** — pick exactly which days to guard
- 🕐 **Configurable no-nap hours** — set your own start and end time
- 💪 **Vibration strength** — Gentle / Medium / Strong
- 🔋 **Battery efficient** — Tier 1 rides free on HR samples the OS was already taking; sensors fully idle outside guard window

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
| **Detection** | HR drop sensitivity (Sensitive=8% / Balanced=13% / Conservative=20%) | Balanced |

### Active days picker

Pressing SELECT on **Active days** opens a day-by-day picker:

- **UP / DOWN** — navigate days
- **SELECT** — toggle a day on/off (`(o)` = active)
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
| Button | Action | Label shown |
|---|---|---|
| UP | Snooze 10 minutes | `snooze 10m ▲` |
| SELECT | Dismiss alarm | `dismiss ●` |
| DOWN | Snooze 30 minutes | `snooze 30m ▼` |

---

## Supported watches

| Platform | Watch | Detection |
|---|---|---|
| **emery** | Pebble Time 2 *(primary target)* | Tier 1 (HR+accel) + Tier 2 fallback |
| **diorite** | Pebble 2 | Tier 1 (HR+accel) + Tier 2 fallback |
| **basalt** | Pebble Time, Pebble Time Steel | Tier 2 only (no HR sensor) |
| **chalk** | Pebble Time Round | Tier 2 only (no HR sensor) |

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
├── package.json              # App config, version, capabilities, target platforms
├── wscript                   # Waf build script
├── src/c/
│   ├── main.c                # Foreground app — alarm UI, snooze, launch handling
│   ├── settings.c            # Settings screen — 5 rows with virtual scroll
│   ├── settings.h
│   ├── days_window.c         # Day picker sub-screen (per-day schedule)
│   ├── days_window.h
│   └── common.h              # Shared constants, persist keys, vibe patterns, helpers
└── worker_src/c/
    └── worker.c              # Background worker — two-tier sleep detection
```

### Key design decisions

**Two-tier detection** — Tier 1 piggybacks on `HealthEventHeartRateUpdate` (zero extra battery — the OS was already waking the CPU for the HR sample). Analysis runs immediately on each event: HR is compared against a rolling 8-sample average, accel magnitude is checked against an EMA baseline. A 5-min fallback timer only peeks HR itself when no OS event arrived recently, covering users with slow or disabled background HR sampling. Tier 2 (HealthService sleep confirmation) runs alongside as a belt-and-suspenders fallback. HR capability is detected at runtime — no `#ifdef` needed.

**Rolling HR buffer** — 8 samples of history (≈40–80 min depending on OS sampling rate). Compared against a rolling average rather than a fixed baseline, so the threshold self-adjusts to your actual resting HR throughout the day.

**`common.h` is the single source of truth** for all persist keys, default values, vibration patterns, and the `is_in_no_nap_window()` helper.

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
| `10` | `HR_BUFFER` | int16_t[8] blob — rolling HR circular buffer |
| `11` | `HR_BUF_IDX` | uint8 — write index into HR buffer |
| `12` | `HR_BUF_COUNT` | uint8 — number of valid HR readings (0–8) |
| `13` | `TRIGGER_STREAK` | uint8 — consecutive Tier 1 trigger count |
| `14` | `ACCEL_AVG` | int32 — EMA of accelerometer magnitude (milli-g) |

---

## Memory usage (emery / Pebble Time 2)

| Component | RAM used | Budget |
|---|---|---|
| Background worker | ~4.9 KB | 10.5 KB |
| Foreground app | ~5.1 KB | 128 KB |

---

## A note on detection latency

| Platform | Method | Typical latency |
|---|---|---|
| Pebble Time 2 / Pebble 2 | HR + accelerometer (Tier 1) | ~10–15 min after nap onset |
| All platforms | HealthService sleep event (Tier 2) | 45–90 min (OS confirmed) |

Tier 1 fires as soon as two consecutive 5-minute samples show HR dropping and movement stopping — typically right as you're settling into sleep, not after a full sleep cycle.

---

## License

MIT
