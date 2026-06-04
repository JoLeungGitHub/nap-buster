# NapBuster ⌚

**v1.7.0** — A Pebble smartwatch app that stops you from napping during the day so you can fall asleep easier at night.

When it detects you're falling asleep during your configured no-nap hours, it vibrates until you wake up and dismiss it.

---

## How it works

NapBuster runs a **background worker** with two-tier sleep detection:

```
Background worker (always running)
    │
    ├─ TIER 1: Early warning — HR + VMC (Pebble Time 2 / Pebble 2 only)
    │
    │   OS fires HealthEventHeartRateUpdate (free — no extra battery)
    │       └──▶ read current HR BPM
    │            read HealthMinuteData.vmc (pre-computed motion intensity)
    │            run analysis:
    │               smoothed HR dropped >N% below anchored awake baseline? (N = sensitivity)
    │               VMC below 100? (very still — not just sitting at desk)
    │               Both true twice in a row? ──▶ double-pulse nudge (~10 min after nap onset)
    │               Nudge ignored and still dropping? ──▶ full alarm (~20 min)
    │
    │   5-min fallback timer (only fires if no HR event arrived recently)
    │       └──▶ same analysis, using HR peek (handles slow/off background sampling)
    │
    ├─ TIER 2: Fallback — Pebble HealthService sleep confirmation (all platforms)
    │       OS confirms sleep ──▶ alarm (45–90 min latency, but reliable)
    │
    └─ Alarm fires ──▶ launch foreground app
                           │
                           ├─ Repeating vibration
                           ├─ SELECT ──▶ dismiss
                           ├─ UP     ──▶ snooze 10 min
                           └─ DOWN   ──▶ snooze 30 min
```

### Why HR is the primary signal

Heart rate is the most reliable indicator of sleep onset. As you fall asleep, your autonomic nervous system shifts from sympathetic to parasympathetic dominance — HR drops, HRV increases. This is a genuine physiological event specific to sleep or deep relaxation approaching it.

**VMC (Vector Magnitude Count)** is a noise filter, not the primary signal. Its job: even if HR dips, you were clearly moving, so it wasn't sleep. Things like sitting down after a walk or post-lunch digestion can temporarily drop HR — VMC rules these out.

### Anchored awake HR baseline

NapBuster maintains an **anchored awake baseline** — your HR when you're awake and resting. Unlike a rolling average that blindly tracks all readings (including ones taken while dozing off), the anchored baseline only updates when you're demonstrably awake and moving at a resting pace.

The baseline update rule uses a **dual-bound VMC gate**:
- **VMC < 50** — too still (possibly already asleep/dozing) → baseline **frozen**
- **50 ≤ VMC < 400** — resting awake zone (desk work, light activity) → baseline updates via EMA
- **VMC ≥ 400** — exercising → baseline **frozen** (prevents post-run HR from inflating threshold)

This means a nap trigger compares against your *actual awake resting HR*, not an average that may have drifted downward during a slow doze. The α=7/8 EMA moves gradually, so brief stillness won't instantly lower the baseline.

A 3-sample smoothing buffer on the current HR reading reduces single-sample noise before the comparison is made.

### VMC vs. raw accelerometer

Previous versions used `accel_service_peek()` (single instantaneous accelerometer snapshot computed by hand). v1.5.0 uses `HealthMinuteData.vmc` — a per-minute aggregate of all accelerometer movement pre-computed by the OS health subsystem. VMC is the same motion signal used by Pebble Health internally and is:
- **Zero extra battery** — computed by the OS regardless of NapBuster
- **Much more stable** — a minute aggregate vs. a single noisy snapshot
- **Properly calibrated** — 0–100 = very still, 100–500 = light movement, 500+ = active

**Battery efficient:** Tier 1 piggybacks on HR samples the OS was already taking. All sensors idle outside your guard window.

---

## Version history

To check your installed version: open NapBuster → long-press SELECT → version shown at the bottom of settings.

| Version | What changed |
|---|---|
| **1.7.0** | Two-stage wake: x1 streak fires a quiet double-pulse nudge; x2 streak fires the full repeating alarm |
| **1.6.0** | Anchored awake HR baseline replaces rolling average — baseline only updates in resting-awake VMC zone (50–400), frozen during sleep onset or exercise; 3-sample HR smoothing buffer; debug display shows `base:` instead of `avg:` |
| **1.5.0** | Replace raw accel peek with `HealthMinuteData.vmc` (pre-computed OS motion signal, zero extra battery, more stable); handle `HealthEventSignificantUpdate`; add `health_service_metric_accessible()` guard before HR reads; debug display shows `vmc:` |
| **1.4.0** | Debug telemetry on GUARDING screen (`HR:68 avg:74 vmc:42 x1`); Detection sensitivity setting (Sensitive / Balanced / Conservative) |
| **1.3.0** | Alarm screen per-button labels (UP/SEL/DN); HR buffer stays warm outside guard window for instant detection at window open |
| **1.2.0** | Tier 1 piggybacks on OS HR events (zero extra battery); 5-min timer skips if HR event arrived recently |
| **1.1.0** | Two-tier detection: HR+motion early warning on Time 2/Pebble 2; HealthService sleep event fallback on all platforms |
| 1.0.0 | Initial release — HealthService sleep event detection only |

---

## Features

- 🔬 **Early nap detection** — HR + VMC catches nap onset in ~10 min (Pebble Time 2 / Pebble 2)
- 🔔 **Two-stage wake** — quiet double-pulse nudge at x1 streak; full repeating alarm only if you don't stir
- 🛡️ **Fallback detection** — Pebble's native sleep confirmation as a safety net on all platforms
- 📳 **Repeating vibration alarm** — keeps buzzing until dismissed
- 💤 **Snooze** — 10 or 30 minutes, re-arms automatically via Wakeup API (survives app close)
- 📅 **Per-day schedule** — pick exactly which days to guard
- 🕐 **Configurable no-nap hours** — set your own start and end time
- 💪 **Vibration strength** — Gentle / Medium / Strong
- 🎛️ **Detection sensitivity** — tune the HR drop threshold to your physiology
- 📊 **Live debug telemetry** — GUARDING screen shows real-time HR, avg, VMC, streak count
- 🔋 **Battery efficient** — VMC is free from the OS; Tier 1 rides on HR samples already being taken

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
| **Detection** | HR drop sensitivity — how large a drop triggers | Balanced |

### Detection sensitivity

| Level | HR drop required | Use when |
|---|---|---|
| **Sensitive** | 8% below rolling average | Missing naps (not triggering enough) |
| **Balanced** | 13% below rolling average | Default — works for most people |
| **Conservative** | 20% below rolling average | Too many false positives |

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

## Debug telemetry

When in GUARDING state, the home screen shows a live readout:

```
HR:68 base:74 vmc:42 x1
```

| Field | Meaning |
|---|---|
| `HR:` | Current heart rate BPM (3-sample smoothed) |
| `base:` | Your anchored awake baseline HR |
| `vmc:` | Vector Magnitude Count — motion intensity this minute (0–100 = still, 500+ = active) |
| `x1` | Consecutive trigger streak (alarm fires at x2 or higher) |

**Tuning guide:**
- If it false-triggers: check `vmc:` when it fires. If VMC is high, you were moving — VMC threshold may need raising. If VMC is low with a small HR drop, try **Conservative** sensitivity or raise `STREAK_TO_FIRE`.
- If it misses naps: check `vmc:` and `HR:` while drowsy. If HR isn't dropping much, try **Sensitive**. If VMC is high during naps (restless sleeper), the VMC gate may be too aggressive.

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
| **emery** | Pebble Time 2 *(primary target)* | Tier 1 (HR + VMC) + Tier 2 fallback |
| **diorite** | Pebble 2 | Tier 1 (HR + VMC) + Tier 2 fallback |
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
│   ├── main.c                # Foreground app — home screen, alarm UI, snooze
│   ├── settings.c            # Settings screen — 6 rows with virtual scroll
│   ├── settings.h
│   ├── days_window.c         # Day picker sub-screen (per-day schedule)
│   ├── days_window.h
│   └── common.h              # Shared constants, persist keys, vibe patterns, helpers
└── worker_src/c/
    └── worker.c              # Background worker v5 — anchored baseline + two-tier sleep detection
```

### Key design decisions

**HR is the primary signal, VMC is the gate.** HR drop is a genuine physiological marker of sleep onset. VMC only rules out cases where HR happened to dip while the user was clearly moving. The algorithm requires both: HR drops AND movement is low, sustained across 2 consecutive HR events.

**`HealthMinuteData.vmc` over raw accelerometer.** VMC is pre-computed by the OS health subsystem — no extra sensor power, no manual `√(x²+y²+z²)` computation, a full-minute aggregate rather than a single noisy instantaneous sample.

**Anchored awake HR baseline, not a rolling average.** A rolling average chases HR downward during a gradual nap onset and never crosses the threshold — this was the core failure mode. The anchored baseline only updates in the resting-awake VMC zone (50–400): frozen during exercise (avoids baseline inflating to 110 bpm after a run) and frozen during stillness (avoids drifting down with sleep onset). A 3-sample smoothing buffer on the current HR reduces single-reading noise before the comparison.

**Piggybacking on OS HR events** — `HealthEventHeartRateUpdate` fires when the OS takes a HR reading anyway. Tier 1 analysis is free; the 5-min fallback timer only peeks HR when the OS hasn't done so recently (slow/off background sampling).

**HR buffer always warm** — sampling continues outside the guard window on HR-capable platforms. `prv_try_launch_foreground()` guards against out-of-window firing, so collecting data outside is safe.

**`common.h` is the single source of truth** for all persist keys, default values, vibration patterns, and `is_in_no_nap_window()`. Worker has its own copies of keys it needs (can't include `common.h`).

**Snooze survives app close** via `wakeup_schedule()`. The OS re-launches the app at the right time.

**Active days** stored as a bitmask (`uint8_t`): bit 0 = Sunday, bit 6 = Saturday. `0x7F` = every day, `0x3E` = weekdays, `0x41` = weekends.

---

## Persist key reference

| Key | Name | Type | Notes |
|---|---|---|---|
| `0` | `ENABLED` | bool | Master on/off |
| `1` | `START_HOUR` | int 0–23 | Guard window start |
| `2` | `END_HOUR` | int 0–23 | Guard window end |
| `3` | `SNOOZE_UNTIL` | time_t | Snooze expiry epoch (0 = none) |
| `4` | `ALARMING` | bool | Alarm currently active |
| `5` | `WAKEUP_ID_SNOOZE` | WakeupId | Scheduled snooze wakeup |
| `7` | `VIBE_STRENGTH` | int | 0=Gentle 1=Medium 2=Strong |
| `8` | `ACTIVE_DAYS` | uint8 bitmask | bit0=Sun..bit6=Sat |
| `10` | `HR_BUFFER` | int16_t[8] blob | Rolling HR circular buffer |
| `11` | `HR_BUF_IDX` | uint8 | Write index into HR buffer |
| `12` | `HR_BUF_COUNT` | uint8 | Number of valid HR readings (0–8) |
| `13` | `TRIGGER_STREAK` | uint8 | Consecutive Tier 1 trigger count |
| `14` | `VMC_EMA` | uint32 | EMA of VMC (Vector Magnitude Count) |
| `15` | `DEBUG_HR` | int16 | Last HR BPM seen by worker |
| `16` | `DEBUG_AVG` | int16 | Last rolling HR average |
| `17` | `DEBUG_ACCEL` | int32 | Last VMC reading |
| `18` | `SENSITIVITY` | int | 0=Sensitive 1=Balanced 2=Conservative |
| `19` | `HR_AWAKE_BASELINE` | int16 | Anchored awake HR baseline (updated only in resting VMC zone) |

---

## Memory usage (emery / Pebble Time 2)

| Component | RAM used | Budget |
|---|---|---|
| Background worker | ~4.5 KB | 10.5 KB |
| Foreground app | ~10.7 KB | 128 KB |

---

## Detection latency

| Platform | Method | Nudge latency | Full alarm latency |
|---|---|---|---|
| Pebble Time 2 / Pebble 2 | HR + VMC (Tier 1) | ~10 min after nap onset | ~20 min (if nudge ignored) |
| All platforms | HealthService sleep event (Tier 2) | — | 45–90 min (OS confirmed) |

Tier 1 requires 2 consecutive HR events showing a drop, so latency depends on the OS HR sampling interval (~10 min default). The nudge fires on the 1st hit; the full alarm fires on the 2nd.

---

## License

MIT
