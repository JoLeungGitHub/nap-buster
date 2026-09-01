# NapBuster ⌚

**v2.5.0** — A Pebble smartwatch app that stops you from napping during the day so you can fall asleep easier at night.

When NapBuster sees sustained signs that you may be dozing during your configured no-nap hours, it gives you a gentle nudge. If stronger evidence continues and you do not appear to respond, it escalates to a repeating alarm.

---

## How it works

NapBuster combines heart rate with recent movement instead of treating any one low reading as sleep:

```text
Timestamped HR event + newest five completed motion minutes
                         │
                         ▼
             3-reading median smoothing
                         │
           fresh, quiet, falling/full-drop HR?
                         │
                         ▼
        ARMED ───────▶ CANDIDATE ───────▶ NUDGED
          ▲                 │                 │
          └── movement / HR recovery ────────┘
                            │
                 4 min valid evidence: nudge
                10 min without response: alarm
```

The background worker has two detection tiers:

- **Tier 1 — early doze detector:** available on watches with a heart-rate sensor. It evaluates each real HR event against an awake baseline and a timestamped summary of the newest five completed `HealthMinuteData` records (VMC and steps).
- **Tier 2 — OS sleep fallback:** Pebble Health's own sleep classification remains a safety net on every supported watch. It is slower and can miss short naps, so it is not the primary early-warning path.

### ARMED → CANDIDATE → NUDGED

**ARMED** is normal monitoring. Three stable readings taken during a fresh, quiet, non-exercise period seed the awake HR baseline. The samples must form a tight cluster, and an isolated outlier is discarded. This works for sedentary users without treating an elevated walking heart rate as their resting baseline. After that, the baseline adapts very slowly only from quiet readings above the soft-drop boundary; it is frozen throughout a possible doze so it cannot follow HR downward and hide the event.

**CANDIDATE** begins when fresh motion data says the wrist has been quiet and smoothed HR is falling below the awake baseline. Evidence advances only between accepted positive samples; ambiguous or negative readings add no time and decay or expire the episode. Duplicate/burst HR events, stale motion, implausible HR, long sampling gaps, steps, exercise, or clear HR recovery cannot turn elapsed wall-clock time into evidence.

**NUDGED** begins after four minutes of valid positive evidence. NapBuster gives a double pulse and looks for a response. Clear movement cancels the episode; quiet negative readings decay the accumulated evidence, and sustained HR recovery cancels it. At ten minutes, the foreground app starts the repeating alarm only if HR has reached the level's full-drop threshold. A merely soft plateau ends after its nudge instead of becoming a false full alarm.

This explicit state machine prevents two important failure modes in older releases: stale sensor values being counted repeatedly, and a candidate accumulating time while no usable data arrived.

### Sensitivity thresholds

Each setting has a **full HR drop** and a **soft HR drop**. A full drop can establish evidence while the wrist is quiet. A soft drop is accepted only with the additional falling-HR pattern, so it can catch the beginning of a gradual slide without making a mildly low HR sufficient on its own.

| Level | Full drop | Soft drop | Use when |
|---|---:|---:|---|
| **Sensitive** | 8% | 4% | NapBuster is missing real naps |
| **Balanced** | 12% | 6% | Default starting point |
| **Conservative** | 16% | 8% | Quiet wakefulness causes false candidates |

The awake baseline is personal and slowly adaptive; these percentages are relative to your own recent resting-awake HR, not a population value.

### Motion and data freshness

NapBuster requests the five minutes ending at the latest completed minute. It records the actual timestamp returned with that history and considers VMC together with step counts. The partial minute currently being accumulated is deliberately excluded, because treating it like a complete low-motion minute biases the detector toward false stillness.

There are no absolute, medically validated VMC cutoffs for dozing. The thresholds in this app are engineering heuristics, and the five-minute summary is used mainly to distinguish sustained quiet from obvious movement or exercise.

### HR and PPI sampling

During normal guarding, NapBuster requests a **120-second HR sample period**. It analyzes the newest raw event-driven BPM, rejects callbacks less than 20 seconds apart, and applies physiological bounds plus median-of-three smoothing. This avoids presenting Pebble's older filtered average as a fresh measurement. While an episode is a CANDIDATE or NUDGED, it temporarily requests faster **20-second HR sampling** and, on supported hardware, PPI/HRV events. These are requests to PebbleOS rather than guaranteed delivery intervals, and the faster candidate probe uses additional sensor power. It is switched off again when the candidate resolves or the guard window closes.

Contiguous, artifact-gated PPI readings can produce an **RMSSD diagnostic** on Pebble Time 2. That number is shown for observation only. It is not a trigger, has no persisted baseline, and should not be treated as a validated sleep-onset signal. The previous inter-burst “HRV drift” path has been removed.

### Baseline migration

v2.5 uses a new detector-state schema. On first run it clears the old HR baseline together with incompatible rolling buffers, streaks, VMC EMA, and HRV state, then recalibrates from stable quiet-awake readings. This deliberately avoids carrying forward a baseline that the previous algorithm may have inflated or allowed to follow a doze.

---

## Features

- 🔬 **Early nap detection** — fresh HR, five-minute VMC/step context, and an explicit response-aware state machine
- 🔔 **Two-stage wake** — gentle nudge after four minutes of valid evidence; full alarm at ten minutes only with a full HR drop
- 🛡️ **Fallback detection** — Pebble Health sleep classification remains a safety net
- 📳 **Repeating vibration alarm** — keeps buzzing until dismissed or snoozed
- 💤 **Snooze** — 10 or 30 minutes via the Wakeup API; an expiry outside the active schedule does not sound an alarm
- 📅 **Per-day schedule** — choose exactly which days to guard, including overnight windows
- 💪 **Vibration strength** — Gentle / Medium / Strong
- 🎛️ **Detection sensitivity** — Sensitive / Balanced / Conservative
- 📊 **Live telemetry** — worker state, detector phase, evidence, HR, baseline, VMC, RMSSD when available, and analysis age
- 🔋 **Candidate-only fast probe** — higher-rate HR/PPI sampling is limited to an active candidate

---

## Settings

Open settings from the main screen with a **long-press on SELECT**.

| Setting | Description | Default |
|---|---|---|
| **Guard** | Master on/off switch | ON |
| **Active days** | Which days of the week to guard | Every day |
| **No-nap from** | Start of the no-nap window | 11:00 |
| **No-nap until** | End of the no-nap window | 23:00 |
| **Wake vibration** | Alarm pulse density | Medium |
| **Detection** | HR-drop sensitivity | Balanced |

For a window crossing midnight, the after-midnight portion belongs to the day on which the window started. For example, Monday-only 22:00–06:00 remains active until 06:00 Tuesday. Setting the same start and end hour means all day; use **Guard** to disable monitoring.

### Active days picker

- **UP / DOWN** — navigate days
- **SELECT** — toggle a day (`(o)` means active)
- **BACK** — save and return

The settings row summarizes the choice as `Every day`, `Weekdays`, `Weekends`, or `N days`.

### Vibration modes

| Mode | Pattern |
|---|---|
| **Gentle** | Two slow soft pulses, long rest |
| **Medium** | Escalating short-to-long bursts |
| **Strong** | Rapid, dense buzzing |

Pebble has no hardware vibration-intensity control, so the modes use different pulse timing and density.

---

## Debug telemetry

While GUARDING, the bottom line reports detector health and the latest accepted analysis:

```text
HR:64/72 v:38 C e:3m a:0m
```

| Field | Meaning |
|---|---|
| `HR:64/72` | Smoothed HR / awake HR baseline, in BPM |
| `v:38` | Latest completed-minute VMC |
| `A`, `C`, `N` | ARMED, CANDIDATE, or NUDGED phase |
| `e:3m` | Whole minutes of accumulated positive evidence (negative readings decay it) |
| `a:0m` | Age of the last completed analysis |

When candidate PPI data is available, `r:42` is inserted after HR; it is RMSSD in milliseconds and is diagnostic only.

Before telemetry is ready, the line gives a more useful status such as `Worker starting...`, `Health unavailable`, `OS sleep fallback`, `HR sensor retrying`, `Calibrating HR...`, or `Waiting for HR...`.

If the age keeps rising while the watch should be guarding, verify that Pebble Health and heart-rate access are enabled in the mobile app. Missing data is intentionally not reused as a new sample.

---

## Controls

### Main screen

| Button | Action |
|---|---|
| SELECT (long press) | Open settings |
| SELECT while snoozed | Cancel snooze and resume guarding |

### Alarm screen

| Button | Action | Label shown |
|---|---|---|
| UP | Snooze 10 minutes | `snooze 10m ▲` |
| SELECT | Dismiss alarm | `dismiss ●` |
| DOWN | Snooze 30 minutes | `snooze 30m ▼` |

---

## Supported watches

| Platform | Watch | Detection |
|---|---|---|
| **emery** | Pebble Time 2 | HR + VMC/steps, candidate PPI diagnostic, OS fallback |
| **diorite** | Pebble 2 | HR + VMC/steps, OS fallback |
| **basalt** | Pebble Time / Time Steel | OS sleep fallback only |
| **chalk** | Pebble Time Round | OS sleep fallback only |

Pebble Health must be enabled in the rePebble mobile app. The PPI diagnostic requires compatible heart-rate hardware and PebbleOS 4.33 or newer; its absence does not disable the HR detector.

---

## Building and installing

### Prerequisites

```bash
# Install pebble-tool (requires Python 3.11)
uv venv ~/.pebble-venv --python 3.11
~/.pebble-venv/bin/pip install pebble-tool
~/.pebble-venv/bin/pebble sdk install latest
```

See [the rePebble SDK guide](https://developer.repebble.com/sdk/) for full setup instructions.

### Build

```bash
cd nap-buster
~/.pebble-venv/bin/pebble build
```

Output: `build/nap-buster.pbw`

### Install to an emulator

```bash
~/.pebble-venv/bin/pebble install --emulator emery
```

### Install to a watch

Enable **Dev Connect** in the rePebble mobile app, then:

```bash
~/.pebble-venv/bin/pebble login
~/.pebble-venv/bin/pebble install --cloudpebble
```

---

## Project structure

```text
nap-buster/
├── package.json
├── wscript
├── src/c/
│   ├── main.c                 Foreground home, alarm, and snooze UI
│   ├── settings.c/.h         Settings screen
│   ├── days_window.c/.h      Active-day picker
│   └── common.h              Shared persist keys and setting helpers
├── worker_src/c/
│   ├── worker.c               Pebble health/sensor adapter
│   └── nap_detector.c/.h      Portable doze state machine
└── tests/
    ├── Makefile               Strict host C11 test target
    └── test_nap_detector.c    Host-side detector scenarios
```

Keeping the detector free of Pebble SDK dependencies makes timestamp, gap, baseline, motion, nudge, recovery, and alarm behavior testable on a normal C compiler.

---

## Persist key reference

| Key | Name | Notes |
|---:|---|---|
| 0–5, 7–8 | Settings/alarm/wakeup | Master switch, schedule, snooze, alarm, vibration, active days |
| 10–12, 14 | Legacy detector state | Cleared during v2.5 schema migration |
| 13 | `TRIGGER_STREAK` | Whole minutes of accumulated positive evidence |
| 15 | `DEBUG_HR` | Last smoothed HR |
| 16 | `DEBUG_AVG` | Awake HR baseline for display |
| 17 | `DEBUG_ACCEL` | Latest completed-minute VMC |
| 18 | `SENSITIVITY` | Sensitive / Balanced / Conservative |
| 19 | `HR_BASELINE` | Persisted awake HR baseline |
| 20–21 | Nudge/dismiss coordination | Foreground request and alarm cooldown |
| 22 | Legacy streak start | Cleared during schema migration |
| 23 | `DEBUG_LAST_TS` | Last completed analysis timestamp |
| 24 | Legacy HRV baseline | Unused by v2.5 |
| 25 | `DEBUG_HRV` | Candidate PPI RMSSD, or −1 |
| 26 | `ALARM_START` | Foreground alarm timestamp / stale-flag watchdog |
| 27 | `DETECTOR_SCHEMA` | Detector persistence schema version |
| 28 | `LAST_NUDGE` | Nudge cooldown timestamp |
| 29 | `DEBUG_PHASE` | ARMED / CANDIDATE / NUDGED |
| 30 | `WORKER_STATUS` | Worker and sensor readiness bitmask |

---

## Detection timing and limits

The **earliest** nudge is after four minutes of accepted positive evidence. The earliest Tier-1 full alarm is at ten minutes when the episode also reaches the selected full HR-drop threshold; a soft-only episode stops after the nudge. Actual time can be longer while the baseline calibrates, when the OS delivers HR less frequently than requested, or when samples are rejected as stale, duplicated, implausible, or separated by a long gap. Pebble Health's fallback classification can be substantially later and may not recognize a short nap.

Wrist actigraphy and optical heart rate cannot prove that someone is awake or asleep. In particular, very quiet wakefulness with an unusually low and falling HR can resemble dozing, while restless dozing, a loose watch, disabled Health access, or missing sensor data can hide it. The nudge-and-response stage reduces false alarms but cannot eliminate them.

NapBuster is a convenience tool, **not a medical or safety device**. Do not rely on it to stay awake while driving, operating machinery, supervising another person, or in any situation where dozing could cause harm.

---

## Version history

| Version | What changed |
|---|---|
| **2.5.0** | Replaced the accumulated v2.x trigger logic with a portable ARMED → CANDIDATE → NUDGED detector; fresh event-driven raw HR with 20-second burst rejection and median-of-three smoothing; stable quiet baseline calibration; completed VMC/step summaries; bounded/decaying evidence; 8/12/16% full and 4/6/8% soft HR-drop thresholds, with soft-only episodes limited to a nudge; candidate-only 20-second sensor probing; PPI RMSSD as diagnostic telemetry only; a new persistence schema and host scenario tests. Fixed overnight active-day ownership, snooze expiry outside the guard window, and a foreground launch race that could turn a nudge into a full alarm. |
| **2.4.1** | Fixed stale foreground alarm state that could prevent later alarms. |
| **2.4.0–2.3.0** | Experimented with an inter-burst PPI drift signal. v2.5 removes it from detection because it was not a valid substitute for contiguous-beat HRV and was not sufficiently validated. |
| **2.2.0–2.0.0** | Added guard-window sensor ownership, time-based nudge/alarm stages, and alarm cooldown handling. |
| **1.8.0–1.4.0** | Introduced anchored HR, VMC motion context, sensitivity controls, and debug telemetry. |
| **1.3.0–1.0.0** | Added alarm controls, background detection, and Pebble Health fallback. |

---

## License

MIT
