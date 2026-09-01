# NapBuster ⌚

**v2.4.1** — A Pebble smartwatch app that stops you from napping during the day so you can fall asleep easier at night.

When it detects you're falling asleep during your configured no-nap hours, it vibrates until you wake up and dismiss it.

---

## How it works

NapBuster runs a **background worker** with two-tier sleep detection:

```
Background worker (always running)
    │
    ├─ TIER 1: Early warning — HRV + HR + VMC (Pebble Time 2; HR-only on Pebble 2)
    │
    │   Inside the guard window the worker requests 120-second HR *and* HRV
    │   sample periods (they share one sensor subscription — no extra wakeups)
    │   (outside the window it rides the OS's own ~10-min samples for free)
    │       └──▶ read current HR BPM
    │            read HRV peak-to-peak intervals (artifact-gated, grouped into
    │              sensor bursts → inter-burst "drift spread")
    │            read HealthMinuteData.vmc (pre-computed motion intensity)
    │            run analysis:
    │               EARLY PATH: drift spread ≤ M% of awake baseline (average
    │                           HR gone metronomic — the doze-off signature)
    │                           AND smoothed HR mildly below baseline
    │               INSURANCE:  smoothed HR dropped >N% below awake baseline
    │               Either path AND VMC below 100? (very still)
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

### Why drift suppression is the primary signal (v2.4.0)

Textbook HRV (rMSSD over adjacent heartbeats) rises at sleep onset. But what this hardware actually hands an app is different: the Pebble Time 2's Goodix sensor reports **up to 4 adjacent-beat RR intervals per sensor burst** (one `HealthEventHRVUpdate` each, no firmware quality filtering), and bursts arrive one sample period (~2 min) apart. A variability statistic over those readings is dominated by the **inter-burst term** — how far your average HR wandered between bursts — not by respiratory beat-to-beat variability.

And that inter-burst *drift* turns out to be a beautiful sleep signal, just inverted from textbook HRV: awake, your average HR wanders constantly (posture, cognition, micro-arousals — measured ~140–165 ms of drift on real wrists); as you doze, your heart goes metronomic and drift **collapses** (~30 ms, appearing *before* full sleep). v2.3.0 coded the textbook direction and its HRV path never fired; v2.4.0 measures what the sensor actually delivers.

Pipeline: each PPI is artifact-gated (physiological range 300–2000 ms, within ±40% of the HR-implied interval), readings ≤10 s apart are grouped into a burst, each burst reduces to its **mean PPI**, and the ring of the last 8 burst means yields the **drift spread** (mean absolute successive difference). Within-burst adjacent-beat variability — the true rMSSD material, expected to rise in sleep — is computed and logged per burst as a future second signal, but doesn't gate yet.

Detection then runs two paths, both gated on stillness:
- **Early path (HRV-primary):** drift spread ≤ 60/50/40% of your awake-drift baseline (Sensitive/Balanced/Conservative) **and** smoothed HR mildly below baseline (roughly half the full drop). Fires early in the doze-off slide, when the HR drop is still shallow.
- **Insurance path:** the v2.x full HR drop (10–24% per sensitivity). Keeps working when HRV data is missing, rejected as artifacts, or the hardware doesn't support it.

**VMC (Vector Magnitude Count)** remains the noise filter for both paths: even if the vitals say "sleepy", movement means you weren't napping. HRV needs a quiet wrist anyway — optical PPI during motion is garbage, which the artifact gate and stillness requirement handle together.

**Hardware note:** HRV requires the Pebble Time 2's sensor (and PebbleOS ≥ 4.33). On Pebble 2 the HRV sample-period request fails cleanly and detection is exactly the v2.2 HR-only behavior.

### Anchored awake HR baseline

NapBuster maintains an **anchored awake baseline** — your HR when you're awake and resting. Unlike a rolling average that blindly tracks all readings (including ones taken while dozing off), the anchored baseline never follows your HR down into a nap.

The update rule is **asymmetric** (v1.8.0):
- **VMC ≥ 400** (exercising) → baseline **frozen** — a run must not inflate the anchor
- **smoothed HR ≥ baseline** → baseline updates **upward** via EMA — an up-move can never be sleep-onset chasing, so it's always allowed; this un-sticks a baseline seeded too low
- **smoothed HR < baseline AND VMC ≥ 50** → baseline updates **downward** via EMA — you're demonstrably awake and moving, so tracking your true resting HR down is safe
- **smoothed HR < baseline AND VMC < 50** → baseline **frozen** — dropping HR while very still is exactly what a nap onset looks like

The baseline is only **seeded** when VMC ≥ 50 (never from a reading taken while you might already be dozing) and clamped to a sane 40–120 BPM range. The α=7/8 EMA moves gradually, so brief conditions won't yank it around.

A 3-sample smoothing buffer on the current HR reading reduces single-sample noise before the comparison is made. At the boosted 120-second cadence that's a ~6-minute smoothing horizon.

### Anchored awake-drift (HRV) baseline

The drift baseline uses the same anchoring philosophy as the HR one — with **down** as the dangerous direction, since sleep *lowers* drift:

- drift **rising** → baseline updates upward whenever the wrist is quiet (VMC < 200) — rising drift is itself wakefulness evidence, but motion-garbage PPIs must not inflate the anchor
- drift **falling** → baseline updates downward only while HR proves wakefulness (smoothed HR ≥ 95% of the HR baseline, quiet wrist)
- seeded only under those clearly-awake conditions; clamped to 5–200 ms

v2.3.0 had this asymmetry mirrored (free downward updates), and the baseline demonstrably chased its user into a doze — 163 ms awake → 35 ms while drowsy — which crippled the trigger margin. With the corrected anchor, dozing at ~30 ms against a ~160 ms awake baseline sits at ~19%, far beyond even the Conservative threshold.

### VMC vs. raw accelerometer

Previous versions used `accel_service_peek()` (single instantaneous accelerometer snapshot computed by hand). v1.5.0 uses `HealthMinuteData.vmc` — a per-minute aggregate of all accelerometer movement pre-computed by the OS health subsystem. VMC is the same motion signal used by Pebble Health internally and is:
- **Zero extra battery** — computed by the OS regardless of NapBuster
- **Much more stable** — a minute aggregate vs. a single noisy snapshot
- **Properly calibrated** — 0–100 = very still, 100–500 = light movement, 500+ = active

**Battery:** HealthService is now subscribed only during the guard window plus a 2-hour lead-in before it opens (to warm the baseline) — not 24/7. The 5-minute fallback timer only runs while that subscription is active, so it no longer wakes the CPU every 5 minutes all day. Inside the window the worker also requests a 120-second HR sample period — this is NapBuster's one real battery spend, and it's what makes near-minute-level detection possible. The boost is cancelled the moment the window closes, on settings changes that close it, and on worker shutdown.

---

## Version history

To check your installed version: open NapBuster → long-press SELECT → version shown at the bottom of settings.

| Version | What changed |
|---|---|
| **2.4.1** | **Critical fix — a stuck flag could permanently disable the alarm.** `ALARMING` was only ever cleared by the alarm screen's own buttons, but BACK isn't bound during an alarm, so leaving that way (or an app timeout/kill) left it set forever and the worker refused every later launch. Seen in the wild as a trigger streak of x26 — ~52 minutes of confirmed dozing — with no alarm. Now: exiting mid-alarm counts as a dismissal (`app_deinit` clears the flag and notifies the worker); the app self-heals a stale flag whenever it opens without alarming; and two worker watchdogs expire stuck state regardless — an `ALARMING` flag older than 30 min (or lacking the new timestamp, i.e. written by an older build) and a launch left unacknowledged for 5 min. Installing this release clears an already-stuck flag automatically |
| **2.4.0** | HRV direction fix, grounded in the sensor's actual behavior (the Goodix driver delivers up to 4 adjacent-beat RRs per burst, one event each — undocumented; verified in PebbleOS source): PPIs are now grouped into bursts and reduced to burst-mean PPIs, so the ring measures pure **inter-burst drift**, which on-wrist data shows is ~140–165 ms awake and collapses to ~30 ms while dozing — the *opposite* of v2.3.0's assumed rMSSD rise. Trigger flipped to drift **suppression** (≤60/50/40% of awake baseline per sensitivity) + the half-strength HR dip; baseline anchoring mirrored (down-moves now need proof of wakefulness — v2.3.0's free-down anchor chased the user into their doze, 163→35 ms); up-moves require a quiet wrist so motion garbage can't inflate the anchor. Within-burst adjacent-beat variability (true rMSSD) is logged per burst as a candidate future signal. A contaminated v2.3.0 baseline self-heals upward within ~25 min of quiet wakefulness |
| **2.3.0** | HRV-primary detection (SDK 4.33 APIs, Pebble Time 2): worker requests an HRV sample period alongside the HR period (shared sensor subscription at the same 120 s — zero extra sensor wakeups); each `HealthEventHRVUpdate` PPI is artifact-gated (300–2000 ms + ±40% of HR-implied interval, since firmware forwards readings unfiltered) into an 8-sample ring; "PPI spread" (mean abs successive difference) vs an inverted-anchored awake-spread baseline adds an early trigger path — spread ≥ 25/40/60% above baseline (per sensitivity) + a *half*-strength HR dip; the full HR-drop path stays as insurance and is the sole path on Pebble 2 / firmware <4.33. Debug line shows `h:spread/base` in HRV mode. Requires PebbleOS ≥ 4.33 |
| **2.2.0** | Battery: HealthService is no longer subscribed 24/7 on HR-capable platforms — it now subscribes only during the guard window plus a 2-hour lead-in beforehand (`WARM_LEAD_HOURS`), so the awake baseline is still warm by the time guarding starts. The 5-minute fallback timer only runs while that subscription is active, instead of waking the CPU every 5 minutes all day regardless of the window. |
| **2.1.0** | Raised HR-drop thresholds (Sensitive 8%→10%, Balanced 13%→16%, Conservative 20%→24%) to reduce false "doze" triggers from ordinary resting relaxation (sitting/lying still, vagal tone alone can drop HR 10-15%). Added SELECT-to-cancel-snooze: pressing SELECT while SNOOZED now cancels the snooze and resumes guarding immediately instead of waiting it out. |
| **2.0.0** | Cadence tuned to 2-minute HR sampling (was 60 s) — halves the in-window battery spend from v1.8.0 while keeping detection latency effectively unchanged, since the two-stage wake thresholds are time-based, not sample-count-based. Detection overhaul carried over from v1.8.0: worker owns the HR cadence (`health_service_set_heart_rate_sample_period`); missing HR/VMC data now freezes the streak instead of resetting it (stale `peek` returns 0 — this was silently zeroing the streak mid-nap); two-stage wake is time-based (nudge ≥4 min sustained, alarm ≥10 min) instead of raw event counts; asymmetric baseline updates (up always unless exercising, down only with awake-zone movement) with guarded seeding and 40–120 clamp; dismiss now actually notifies the worker + 10-min re-fire cooldown; out-of-window HR subscription no longer torn down after 60 s (warm baseline for real); HR capability probe self-heals; nudge cooldown (10 min); debug line shows analysis age |
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

- 🔬 **Early nap detection** — HRV + HR + VMC at a 2-minute cadence catches nap onset in ~5–10 min (Pebble Time 2; HR + VMC on Pebble 2)
- 💓 **HRV-primary triggering** — collapsing heart-rate drift (your pulse going metronomic is the front edge of the doze-off slide) fires with only a mild HR dip; the full HR-drop path remains as insurance
- 🔔 **Two-stage wake** — quiet double-pulse nudge after ~4 min of sustained evidence; full repeating alarm at ~10 min if you don't stir
- 🛡️ **Fallback detection** — Pebble's native sleep confirmation as a safety net on all platforms
- 📳 **Repeating vibration alarm** — keeps buzzing until dismissed
- 💤 **Snooze** — 10 or 30 minutes, re-arms automatically via Wakeup API (survives app close)
- 📅 **Per-day schedule** — pick exactly which days to guard
- 🕐 **Configurable no-nap hours** — set your own start and end time
- 💪 **Vibration strength** — Gentle / Medium / Strong
- 🎛️ **Detection sensitivity** — tune the HR drop threshold to your physiology
- 📊 **Live debug telemetry** — GUARDING screen shows real-time HR, baseline, VMC, streak, analysis age
- 🔋 **Battery-aware** — HealthService only subscribes during the guard window plus a 2h lead-in beforehand; fully idle the rest of the day. Boosted HR sampling runs only while guarding

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

When in GUARDING state, the home screen shows a live readout. In HRV mode (Pebble Time 2, once enough bursts are collected):

```
HR:64/71 h:145/160 v:38 x0 0m
```

| Field | Meaning |
|---|---|
| `HR:64/71` | Smoothed heart rate / anchored awake HR baseline (BPM) |
| `h:145/160` | Current drift spread / anchored awake-drift baseline (ms) — the HRV signal. Awake it sits high (~140–165); **it collapsing toward ~30 means you're drifting off** |
| `v:38` | Vector Magnitude Count — motion intensity this minute (0–100 = still, 500+ = active) |
| `x0` | Consecutive positive-cycle streak |
| `0m` | Minutes since the worker last completed an analysis cycle |

Before HRV is warm (fewer than 5 completed bursts, or unsupported hardware), the line falls back to the HR-only format `HR:68 base:74 vmc:42 x1 2m`.

**The age field is the first thing to check.** While guarding it should read `0m`–`2m` (boosted 120 s cadence). If it keeps climbing, no analysis is running — HR data isn't reaching the worker (Pebble Health or HR disabled in the mobile app, or the watch rejected the sample-period request).

**Tuning guide:**
- If it false-triggers: check `v:` when it fires. If VMC is high, you were moving — VMC threshold may need raising. If it fired on the HRV path (`h:` current far below baseline while you were awake — e.g. calmly reading), try **Conservative** sensitivity (tightens both the drift-suppression and HR-drop requirements) or raise `ALARM_AFTER_SECS` in `worker.c`.
- If it misses naps: watch `h:` while drowsy — the current value should sink well below half the baseline. If the baseline itself looks too low (e.g. `h:30/40` while dozing), it hasn't recovered from quiet time yet; it rises again with normal awake activity. If the line stays in HR-only format, the burst window may be starved by artifact rejection — the HR insurance path still applies, so also consider **Sensitive**. If VMC is high during naps (restless sleeper), the VMC gate may be too aggressive — though since v1.8.0 a single restless minute no longer resets the streak (the VMC trend has to rise too).

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
| **emery** | Pebble Time 2 *(primary target)* | Tier 1 (HRV + HR + VMC) + Tier 2 fallback |
| **diorite** | Pebble 2 | Tier 1 (HR + VMC — no HRV hardware) + Tier 2 fallback |
| **basalt** | Pebble Time, Pebble Time Steel | Tier 2 only (no HR sensor) |
| **chalk** | Pebble Time Round | Tier 2 only (no HR sensor) |

> Requires Pebble Health to be enabled in the rePebble mobile app (Devices → Health).
> v2.3.0 builds against SDK 4.33+ and needs PebbleOS ≥ 4.33 on the watch; the HRV path activates only where the hardware supports it and degrades to HR-only everywhere else.

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

Output: `build/nap-buster.pbw`

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

**HR buffer warm at window open** — on HR-capable platforms the health subscription and fallback timer now start 2 hours before the guard window opens (not 24/7), so the baseline is already real by the time guarding actually starts, without burning battery the rest of the day. `prv_try_launch_foreground()` guards against out-of-window firing, so collecting data during the lead-in is safe — and a nap already in progress when the window opens alarms within minutes.

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
| `24` | `HRV_BASELINE` | int16 | Anchored awake drift baseline (ms) |
| `25` | `DEBUG_HRV` | int | Last drift spread (ms), −1 = unavailable |
| `26` | `ALARM_START` | time_t | When the foreground alarm began — stale-flag watchdog |

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
| Pebble Time 2 | HRV + HR + VMC, 120 s cadence (Tier 1 early path) | ~4–6 min sustained evidence | ~10–12 min (if nudge ignored) |
| Pebble Time 2 / Pebble 2 | HR + VMC, boosted 120 s cadence (Tier 1 insurance path) | ~4–6 min sustained evidence | ~10–12 min (if nudge ignored) |
| Pebble Time 2 / Pebble 2 | HR + VMC, boost rejected by OS (Tier 1 degraded) | ~10–20 min | ~20–30 min |
| All platforms | HealthService sleep event (Tier 2) | — | 45–90 min (OS confirmed; short naps may never classify) |

Tier 1 fires on *sustained* evidence: at least 2 positive cycles spanning ≥4 minutes for the nudge, at least 3 spanning ≥10 minutes for the full alarm. The cycle thresholds are identical for both paths — what the HRV path buys is *reaching* threshold sooner in the physiological timeline, because HRV rises within the first minutes of drifting off while a full 16% HR drop can take considerably longer (or never quite arrive for a light doze). The HRV path also needs its 5-sample spread window warm — about 10 minutes of accepted PPIs after the guard window opens on day one; after that the persisted baseline makes it immediate.

---

## License

MIT
