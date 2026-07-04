# NapBuster ⌚

**v1.8.0** — A Pebble smartwatch app that stops you from napping during the day so you can fall asleep easier at night.

When it detects you're falling asleep during your configured no-nap hours, it vibrates until you wake up and dismiss it.

---

## How it works

NapBuster runs a **background worker** with two-tier sleep detection:

```
Background worker (always running)
    │
    ├─ TIER 1: Early warning — HR + VMC (Pebble Time 2 / Pebble 2 only)
    │
    │   Inside the guard window the worker requests a 60-second HR sample
    │   period, so HealthEventHeartRateUpdate arrives ~once a minute
    │   (outside the window it rides the OS's own ~10-min samples for free)
    │       └──▶ read current HR BPM
    │            read HealthMinuteData.vmc (pre-computed motion intensity)
    │            run analysis:
    │               smoothed HR dropped >N% below anchored awake baseline? (N = sensitivity)
    │               VMC below 100? (very still — not just sitting at desk)
    │               Missing HR or VMC data? ──▶ cycle skipped, streak preserved
    │               Sustained ≥4 min (≥2 cycles)  ──▶ double-pulse nudge
    │               Sustained ≥10 min (≥3 cycles) ──▶ full alarm
    │
    │   5-min fallback timer (only fires if no HR event arrived recently)
    │       └──▶ same analysis, using HR peek (handles boost-rejected sampling)
    │
    ├─ TIER 2: Fallback — Pebble HealthService sleep confirmation (all platforms)
    │       OS confirms sleep ──▶ alarm (45–90 min latency; short naps may
    │       never be classified — this is a safety net, not the main path)
    │
    └─ Alarm fires ──▶ launch foreground app
                           │
                           ├─ Repeating vibration
                           ├─ SELECT ──▶ dismiss (10-min re-fire cooldown)
                           ├─ UP     ──▶ snooze 10 min
                           └─ DOWN   ──▶ snooze 30 min
```

### Why HR is the primary signal

Heart rate is the most reliable indicator of sleep onset. As you fall asleep, your autonomic nervous system shifts from sympathetic to parasympathetic dominance — HR drops, HRV increases. This is a genuine physiological event specific to sleep or deep relaxation approaching it.

**VMC (Vector Magnitude Count)** is a noise filter, not the primary signal. Its job: even if HR dips, you were clearly moving, so it wasn't sleep. Things like sitting down after a walk or post-lunch digestion can temporarily drop HR — VMC rules these out.

### Anchored awake HR baseline

NapBuster maintains an **anchored awake baseline** — your HR when you're awake and resting. Unlike a rolling average that blindly tracks all readings (including ones taken while dozing off), the anchored baseline never follows your HR down into a nap.

The update rule is **asymmetric** (v1.8.0):
- **VMC ≥ 400** (exercising) → baseline **frozen** — a run must not inflate the anchor
- **smoothed HR ≥ baseline** → baseline updates **upward** via EMA — an up-move can never be sleep-onset chasing, so it's always allowed; this un-sticks a baseline seeded too low
- **smoothed HR < baseline AND VMC ≥ 50** → baseline updates **downward** via EMA — you're demonstrably awake and moving, so tracking your true resting HR down is safe
- **smoothed HR < baseline AND VMC < 50** → baseline **frozen** — dropping HR while very still is exactly what a nap onset looks like

The baseline is only **seeded** when VMC ≥ 50 (never from a reading taken while you might already be dozing) and clamped to a sane 40–120 BPM range. The α=7/8 EMA moves gradually, so brief conditions won't yank it around.

A 3-sample smoothing buffer on the current HR reading reduces single-sample noise before the comparison is made. At the boosted 60-second cadence that's a ~3-minute smoothing horizon.

### VMC vs. raw accelerometer

Previous versions used `accel_service_peek()` (single instantaneous accelerometer snapshot computed by hand). v1.5.0 uses `HealthMinuteData.vmc` — a per-minute aggregate of all accelerometer movement pre-computed by the OS health subsystem. VMC is the same motion signal used by Pebble Health internally and is:
- **Zero extra battery** — computed by the OS regardless of NapBuster
- **Much more stable** — a minute aggregate vs. a single noisy snapshot
- **Properly calibrated** — 0–100 = very still, 100–500 = light movement, 500+ = active

**Battery:** outside the guard window Tier 1 piggybacks on HR samples the OS was already taking (zero extra cost). *Inside* the window the worker requests a 60-second HR sample period — this is NapBuster's one real battery spend, and it's what makes minute-level detection possible. The boost is cancelled the moment the window closes, on settings changes that close it, and on worker shutdown.

---

## Version history

To check your installed version: open NapBuster → long-press SELECT → version shown at the bottom of settings.

| Version | What changed |
|---|---|
| **1.8.0** | Detection overhaul — fixes "never fires" and "nudge spam": worker owns the HR cadence (60 s sample period inside the window via `health_service_set_heart_rate_sample_period`); missing HR/VMC data now freezes the streak instead of resetting it (stale `peek` returns 0 — this was silently zeroing the streak mid-nap); two-stage wake is time-based (nudge ≥4 min sustained, alarm ≥10 min) instead of raw event counts; asymmetric baseline updates (up always unless exercising, down only with awake-zone movement) with guarded seeding and 40–120 clamp; dismiss now actually notifies the worker + 10-min re-fire cooldown; out-of-window HR subscription no longer torn down after 60 s (warm baseline for real); HR capability probe self-heals; nudge cooldown (10 min); debug line shows analysis age |
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

- 🔬 **Early nap detection** — HR + VMC at a 1-minute cadence catches nap onset in ~5–10 min (Pebble Time 2 / Pebble 2)
- 🔔 **Two-stage wake** — quiet double-pulse nudge after ~4 min of sustained evidence; full repeating alarm at ~10 min if you don't stir
- 🛡️ **Fallback detection** — Pebble's native sleep confirmation as a safety net on all platforms
- 📳 **Repeating vibration alarm** — keeps buzzing until dismissed
- 💤 **Snooze** — 10 or 30 minutes, re-arms automatically via Wakeup API (survives app close)
- 📅 **Per-day schedule** — pick exactly which days to guard
- 🕐 **Configurable no-nap hours** — set your own start and end time
- 💪 **Vibration strength** — Gentle / Medium / Strong
- 🎛️ **Detection sensitivity** — tune the HR drop threshold to your physiology
- 📊 **Live debug telemetry** — GUARDING screen shows real-time HR, baseline, VMC, streak, analysis age
- 🔋 **Battery-aware** — sensors idle outside the window; boosted HR sampling runs only while guarding

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
| **Sensitive** | 8% below awake baseline | Missing naps (not triggering enough) |
| **Balanced** | 13% below awake baseline | Default — works for most people |
| **Conservative** | 20% below awake baseline | Too many false positives |

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
HR:68 base:74 vmc:42 x1 2m
```

| Field | Meaning |
|---|---|
| `HR:` | Current heart rate BPM (3-sample smoothed) |
| `base:` | Your anchored awake baseline HR |
| `vmc:` | Vector Magnitude Count — motion intensity this minute (0–100 = still, 500+ = active) |
| `x1` | Consecutive positive-cycle streak |
| `2m` | Minutes since the worker last completed an analysis cycle |

**The age field is the first thing to check.** While guarding it should read `0m`–`1m` (boosted 60 s cadence). If it keeps climbing, no analysis is running — HR data isn't reaching the worker (Pebble Health or HR disabled in the mobile app, or the watch rejected the sample-period request).

**Tuning guide:**
- If it false-triggers: check `vmc:` when it fires. If VMC is high, you were moving — VMC threshold may need raising. If VMC is low with a small HR drop, try **Conservative** sensitivity or raise `ALARM_AFTER_SECS` in `worker.c`.
- If it misses naps: check `vmc:` and `HR:` while drowsy. If HR isn't dropping much, try **Sensitive**. If VMC is high during naps (restless sleeper), the VMC gate may be too aggressive — though since v1.8.0 a single restless minute no longer resets the streak (the VMC trend has to rise too).

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
    └── worker.c              # Background worker v6 — owned HR cadence + two-tier sleep detection
```

### Key design decisions

**HR is the primary signal, VMC is the gate.** HR drop is a genuine physiological marker of sleep onset. VMC only rules out cases where HR happened to dip while the user was clearly moving. The algorithm requires both — HR dropped AND movement low — sustained over both a minimum number of cycles and a minimum wall-clock time.

**Own the sample cadence while guarding.** The OS's default background HR sampling is ~every 10 minutes, and *slower* during long stillness — precisely when a nap is happening. Inside the window the worker calls `health_service_set_heart_rate_sample_period(60)` so evidence arrives every minute; outside the window it rides the free OS samples. This was the single biggest v1.7 → v1.8 fix: without it, detection latency was unpredictable and the promised "~10 min" was unachievable.

**Missing data freezes, never resets.** `health_service_peek_current_value(HeartRateBPM)` returns 0 once the filtered sample is >15 min old, and recent `HealthMinuteData` records can be invalid or lagging. Any such cycle is skipped with all detection state intact. Treating "no data" as "user is awake" was v1.7's fatal bug — the streak was zeroed mid-nap by stale reads, so the full alarm could effectively never fire.

**Time-based two-stage wake.** Nudge and alarm require sustained wall-clock evidence (≥4 min / ≥10 min), not raw event counts, so behavior is the same whether events arrive every minute (boost accepted) or every ten (boost rejected). Nudges have a 10-minute cooldown; a dismissed alarm sets a 10-minute re-fire cooldown (the OS's sleep classification stays stale for a while after you actually wake).

**`HealthMinuteData.vmc` over raw accelerometer.** VMC is pre-computed by the OS health subsystem — no extra sensor power, no manual `√(x²+y²+z²)` computation, a full-minute aggregate rather than a single noisy instantaneous sample. The stillness gate passes if either the current minute *or* the VMC trend (EMA) is below threshold, so one restless minute mid-nap doesn't discard the streak.

**Anchored awake HR baseline, not a rolling average.** A rolling average chases HR downward during a gradual nap onset and never crosses the threshold. The anchored baseline updates upward freely (unless exercising, which would inflate it) and downward only with awake-zone movement (VMC ≥ 50) — so it converges to your true resting HR but can't follow a doze down. Seeding requires movement evidence; values are clamped to 40–120 BPM.

**HR buffer always warm** — on HR-capable platforms the health subscription and fallback timer run permanently (piggybacked events cost nothing), so the baseline is real at the moment the window opens. `prv_try_launch_foreground()` guards against out-of-window firing, so collecting data outside is safe — and a nap already in progress when the window opens alarms within minutes.

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
| `12` | `HR_BUF_COUNT` | uint8 | Number of valid HR readings (0–3) |
| `13` | `TRIGGER_STREAK` | uint8 | Consecutive Tier 1 positive cycles |
| `14` | `VMC_EMA` | uint32 | EMA of VMC (Vector Magnitude Count) |
| `15` | `DEBUG_HR` | int16 | Last smoothed HR BPM seen by worker |
| `16` | `DEBUG_AVG` | int16 | Anchored awake baseline (display) |
| `17` | `DEBUG_ACCEL` | int32 | Last VMC reading |
| `18` | `SENSITIVITY` | int | 0=Sensitive 1=Balanced 2=Conservative |
| `19` | `HR_AWAKE_BASELINE` | int16 | Anchored awake HR baseline |
| `20` | `NUDGE_PENDING` | bool | Worker requests a nudge pulse from foreground |
| `21` | `LAST_DISMISS` | time_t | Last alarm dismissal — worker re-fire cooldown |
| `22` | `STREAK_START` | time_t | Start of the current positive-cycle streak |
| `23` | `DEBUG_LAST_TS` | time_t | When the worker last completed an analysis |

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
| Pebble Time 2 / Pebble 2 | HR + VMC, boosted 60 s cadence (Tier 1) | ~4–5 min sustained evidence | ~10–11 min (if nudge ignored) |
| Pebble Time 2 / Pebble 2 | HR + VMC, boost rejected by OS (Tier 1 degraded) | ~10–20 min | ~20–30 min |
| All platforms | HealthService sleep event (Tier 2) | — | 45–90 min (OS confirmed; short naps may never classify) |

Tier 1 fires on *sustained* evidence: at least 2 positive cycles spanning ≥4 minutes for the nudge, at least 3 spanning ≥10 minutes for the full alarm. Add the physiological lag between closing your eyes and your HR actually settling (~5 min) to get wall-clock time from nap onset.

---

## License

MIT
