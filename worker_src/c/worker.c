/**
 * worker.c — NapBuster Background Worker v6
 *
 * Two-tier sleep detection:
 *
 *   TIER 1 (HR-capable platforms: emery/diorite)
 *   ─────────────────────────────────────────────
 *   Inside the guard window the worker OWNS the HR cadence: it requests a
 *   120-second heart-rate sample period (health_service_set_heart_rate_sample_period),
 *   so HealthEventHeartRateUpdate arrives ~once every 2 minutes instead of the OS
 *   default of once every ~10 minutes (and even less often during long
 *   stillness — exactly when a nap is happening). Outside the window the
 *   boost is cancelled and Tier 1 rides free on the OS's own background
 *   samples to keep the awake baseline warm.
 *
 *   Each analysis cycle needs BOTH a valid HR reading and a valid VMC minute
 *   record. Missing data FREEZES the detection state (skip cycle) — it never
 *   resets the streak. Only positive evidence of wakefulness (valid HR that
 *   isn't dropped, or clear movement) resets it. This matters because
 *   health_service_peek_current_value(HeartRateBPM) returns 0 once the
 *   filtered sample is >15 min old, which used to zero the streak mid-nap.
 *
 *   Two-stage wake, time-based (cadence-independent):
 *     • nudge:      ≥2 positive cycles sustained ≥4 min  → double-pulse launch
 *     • full alarm: ≥3 positive cycles sustained ≥10 min → repeating alarm
 *   With the time-based thresholds this holds regardless of cadence: nudge
 *   still fires once ~4 min of sustained evidence accumulates, alarm once
 *   ~10 min does — 120 s samples just mean up to one extra cadence tick
 *   (~2 min) of slop versus the 60 s cadence before crossing that mark. If
 *   the OS
 *   rejects the boost (default 10-min cadence) it degrades to ~10 min nudge,
 *   ~20 min alarm — the same latency v1.7 promised but could not deliver.
 *
 *   TIER 2 (ALL platforms)
 *   ─────────────────────────────────────────────
 *   HealthService sleep-activity confirmation as a fallback (slow: the OS
 *   classifies sleep with 45-90+ min latency, and short naps may never be
 *   classified at all). On basalt/chalk this is the only tier.
 *
 * Lifecycle:
 *   HR-capable  → HealthService ALWAYS subscribed (piggybacked events keep the
 *                 awake baseline warm), 5-min fallback timer always armed.
 *                 HR boost only inside the window.
 *   non-HR      → HealthService subscribed only inside the window (Tier 2).
 *   A 60-second timer drives window-boundary transitions either way.
 *
 * SDK notes:
 *   - worker_launch_app() returns void
 *   - WakeupId / wakeup_schedule() NOT available in worker context
 *   - HealthService IS fully available in workers (incl. set_heart_rate_sample_period)
 *   - peek_current_value(HeartRateBPM) self-limits to samples <15 min old
 *   - health_service_get_minute_history() records may be invalid/lagging —
 *     search back several minutes for the newest valid VMC
 */

#include <pebble_worker.h>

// ─── Shared Persist Keys ──────────────────────────────────────────────────────

#define PERSIST_KEY_ENABLED            0
#define PERSIST_KEY_START_HOUR         1
#define PERSIST_KEY_END_HOUR           2
#define PERSIST_KEY_SNOOZE_UNTIL       3
#define PERSIST_KEY_ALARMING           4
#define PERSIST_KEY_ACTIVE_DAYS        8  // uint8 bitmask bit0=Sun..bit6=Sat

// Tier-1 state (persisted across worker restarts)
#define PERSIST_KEY_HR_BUFFER         10  // int16_t[HR_BUF_SIZE] blob
#define PERSIST_KEY_HR_BUF_IDX        11  // uint8_t write index
#define PERSIST_KEY_HR_BUF_COUNT      12  // uint8_t valid count (max HR_BUF_SIZE)
#define PERSIST_KEY_TRIGGER_STREAK    13  // uint8_t consecutive positive cycles
#define PERSIST_KEY_VMC_EMA           14  // uint32_t EMA of VMC

// Tier-1 debug telemetry (written each analysis cycle, read by foreground app)
#define PERSIST_KEY_DEBUG_HR          15  // int16: last smoothed HR BPM
#define PERSIST_KEY_DEBUG_AVG         16  // int16: anchored awake baseline
#define PERSIST_KEY_DEBUG_ACCEL       17  // int32: last VMC reading

// Detection sensitivity (shared with foreground app)
#define PERSIST_KEY_SENSITIVITY       18  // int: 0=Sensitive 1=Balanced 2=Conservative
#define DEFAULT_SENSITIVITY           1   // Balanced

#define PERSIST_KEY_HR_BASELINE       19  // int16: anchored awake HR baseline
#define PERSIST_KEY_NUDGE_PENDING     20  // bool: worker set nudge, foreground should pulse+clear
#define PERSIST_KEY_LAST_DISMISS      21  // time_t: last alarm dismissal (written by foreground)
#define PERSIST_KEY_STREAK_START      22  // time_t: first positive cycle of current streak
#define PERSIST_KEY_DEBUG_LAST_TS     23  // time_t: when the last analysis cycle completed

// ─── Defaults ─────────────────────────────────────────────────────────────────

#define DEFAULT_ENABLED                1
#define DEFAULT_START_HOUR             11
#define DEFAULT_END_HOUR               23
#define DEFAULT_ACTIVE_DAYS            0x7F  // every day

// ─── Messages ─────────────────────────────────────────────────────────────────

#define WORKER_MSG_SLEEP_DETECTED      0
#define WORKER_MSG_NAP_NUDGE           1  // gentle nudge request
#define APP_MSG_SNOOZE_10              10
#define APP_MSG_SNOOZE_30              11
#define APP_MSG_DISMISS                12
#define APP_MSG_SETTINGS_CHANGED       13

// ─── Tier-1 Constants ─────────────────────────────────────────────────────────

#define HR_BUF_SIZE           3       // smoothing buffer (~6 min at boosted 120s cadence)
#define VMC_STILL_THRESH      100     // VMC below this = still enough to be asleep
#define VMC_BASELINE_MIN      50      // VMC needed for a DOWNWARD baseline update / seed
#define VMC_BASELINE_MAX      400     // VMC at/above this = exercising, baseline frozen
#define VMC_EMA_ALPHA         4       // VMC EMA weight
#define VMC_LOOKBACK_MIN      10      // minutes of minute-history searched for a valid VMC
#define BASELINE_MIN_BPM      40      // sanity clamp for the awake baseline
#define BASELINE_MAX_BPM      120
#define HR_BOOST_PERIOD_SECS  120     // in-window HR sample period request

// Two-stage wake: counts AND sustained time both required (cadence-independent)
#define NUDGE_MIN_COUNT       2
#define NUDGE_AFTER_SECS      240     // ~4 min of sustained evidence → nudge
#define ALARM_MIN_COUNT       3
#define ALARM_AFTER_SECS      600     // ~10 min of sustained evidence → full alarm
#define NUDGE_COOLDOWN_SECS   600     // at most one nudge per 10 min
#define DISMISS_COOLDOWN_SECS 600     // after a dismiss, no nudge/alarm for 10 min

#define SAMPLE_INTERVAL_MS    300000  // 5 minutes (fallback timer)
#define SAMPLE_INTERVAL_SECS  300
#define WINDOW_CHECK_MS       60000   // 1 minute window boundary check

// ─── State ────────────────────────────────────────────────────────────────────

// True once worker_launch_app() called and haven't gotten a dismiss/awake yet
static bool s_launch_pending    = false;

// True when HealthService is currently subscribed
static bool s_health_subscribed = false;

// True if this platform has usable HR data (runtime-detected, self-healing)
static bool s_hr_capable        = false;

// True while our boosted HR sample period request is active
static bool s_hr_boosted        = false;

// True while inside the guard window (drives open/close transitions)
static bool s_window_active     = false;

// 60-second window boundary timer
static AppTimer *s_window_timer  = NULL;

// 5-minute Tier-1 fallback sample timer (only armed if s_hr_capable)
static AppTimer *s_sample_timer  = NULL;

// Tier-1 HR smoothing buffer
static int16_t  s_hr_buf[HR_BUF_SIZE];
static uint8_t  s_hr_buf_idx   = 0;
static uint8_t  s_hr_buf_count = 0;

// Tier-1 streak: consecutive positive cycles + when the run started
static uint8_t  s_trigger_streak = 0;
static time_t   s_streak_start   = 0;

// Last nudge time (RAM only — worker is long-lived)
static time_t   s_last_nudge_time = 0;

// Tier-1 VMC EMA (pre-computed motion from HealthMinuteData)
static uint32_t s_vmc_ema       = 0;

// Anchored awake HR baseline. Updates upward freely (unless exercising);
// updates downward only with awake-zone movement. Never chases sleep onset.
static int16_t  s_hr_awake_baseline = 0;  // 0 = not yet established

// Timestamp of last HealthEventHeartRateUpdate (0 = never)
static time_t   s_last_hr_event_time = 0;

// ─── Setting Helpers ──────────────────────────────────────────────────────────

static bool prv_get_enabled(void) {
    if (!persist_exists(PERSIST_KEY_ENABLED)) return DEFAULT_ENABLED;
    return (bool)persist_read_int(PERSIST_KEY_ENABLED);
}

static int prv_get_start_hour(void) {
    if (!persist_exists(PERSIST_KEY_START_HOUR)) return DEFAULT_START_HOUR;
    return persist_read_int(PERSIST_KEY_START_HOUR);
}

static int prv_get_end_hour(void) {
    if (!persist_exists(PERSIST_KEY_END_HOUR)) return DEFAULT_END_HOUR;
    return persist_read_int(PERSIST_KEY_END_HOUR);
}

/** Return the HR-drop threshold percentage based on persisted sensitivity. */
static int prv_get_hr_drop_pct(void) {
    int level = persist_exists(PERSIST_KEY_SENSITIVITY)
                ? persist_read_int(PERSIST_KEY_SENSITIVITY)
                : DEFAULT_SENSITIVITY;
    switch (level) {
        case 0: return 92;  // Sensitive:     8% drop
        case 2: return 80;  // Conservative: 20% drop
        default: return 87; // Balanced:     13% drop
    }
}

static bool prv_is_in_window(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    // Active-days bitmask check (bit 0=Sun ... bit 6=Sat).
    // A stored 0 would mean "never guard" — treat it as corrupt and use default.
    uint8_t active_days = persist_exists(PERSIST_KEY_ACTIVE_DAYS)
        ? (uint8_t)persist_read_int(PERSIST_KEY_ACTIVE_DAYS)
        : DEFAULT_ACTIVE_DAYS;
    active_days &= 0x7F;
    if (active_days == 0) active_days = DEFAULT_ACTIVE_DAYS;
    if (!((active_days >> t->tm_wday) & 1)) return false;

    int hour  = t->tm_hour;
    int start = prv_get_start_hour();
    int end   = prv_get_end_hour();

    if (start <= end) {
        return (hour >= start && hour < end);
    } else {
        return (hour >= start || hour < end);
    }
}

// ─── Guard Helpers ────────────────────────────────────────────────────────────

static bool prv_is_snoozed(void) {
    if (!persist_exists(PERSIST_KEY_SNOOZE_UNTIL)) return false;
    time_t snooze_until = (time_t)persist_read_int(PERSIST_KEY_SNOOZE_UNTIL);
    if (snooze_until == 0) return false;
    return time(NULL) < snooze_until;
}

static bool prv_is_already_alarming(void) {
    if (!persist_exists(PERSIST_KEY_ALARMING)) return false;
    return (bool)persist_read_int(PERSIST_KEY_ALARMING);
}

/** True within DISMISS_COOLDOWN_SECS of the user dismissing an alarm. */
static bool prv_dismiss_cooldown_active(void) {
    if (!persist_exists(PERSIST_KEY_LAST_DISMISS)) return false;
    time_t last = (time_t)persist_read_int(PERSIST_KEY_LAST_DISMISS);
    time_t now = time(NULL);
    if (last <= 0 || now < last) return false;
    return (now - last) < DISMISS_COOLDOWN_SECS;
}

// ─── Tier-1 Persist Helpers ──────────────────────────────────────────────────

static void prv_save_hr_state(void) {
    persist_write_data(PERSIST_KEY_HR_BUFFER, s_hr_buf,
                       sizeof(int16_t) * HR_BUF_SIZE);
    persist_write_int(PERSIST_KEY_HR_BUF_IDX,      s_hr_buf_idx);
    persist_write_int(PERSIST_KEY_HR_BUF_COUNT,    s_hr_buf_count);
    persist_write_int(PERSIST_KEY_TRIGGER_STREAK,  s_trigger_streak);
    persist_write_int(PERSIST_KEY_STREAK_START,    (int)s_streak_start);
    persist_write_int(PERSIST_KEY_VMC_EMA,         (int)s_vmc_ema);
    persist_write_int(PERSIST_KEY_HR_BASELINE,     (int)s_hr_awake_baseline);
}

static void prv_load_hr_state(void) {
    if (persist_exists(PERSIST_KEY_HR_BUFFER)) {
        persist_read_data(PERSIST_KEY_HR_BUFFER, s_hr_buf,
                          sizeof(int16_t) * HR_BUF_SIZE);
    }
    s_hr_buf_idx      = persist_exists(PERSIST_KEY_HR_BUF_IDX)
                        ? (uint8_t)persist_read_int(PERSIST_KEY_HR_BUF_IDX)     : 0;
    s_hr_buf_count    = persist_exists(PERSIST_KEY_HR_BUF_COUNT)
                        ? (uint8_t)persist_read_int(PERSIST_KEY_HR_BUF_COUNT)   : 0;
    s_trigger_streak  = persist_exists(PERSIST_KEY_TRIGGER_STREAK)
                        ? (uint8_t)persist_read_int(PERSIST_KEY_TRIGGER_STREAK) : 0;
    s_streak_start    = persist_exists(PERSIST_KEY_STREAK_START)
                        ? (time_t)persist_read_int(PERSIST_KEY_STREAK_START)    : 0;
    s_vmc_ema         = persist_exists(PERSIST_KEY_VMC_EMA)
                        ? (uint32_t)persist_read_int(PERSIST_KEY_VMC_EMA)       : 0;
    s_hr_awake_baseline = persist_exists(PERSIST_KEY_HR_BASELINE)
                          ? (int16_t)persist_read_int(PERSIST_KEY_HR_BASELINE) : 0;

    // Guard restored values
    if (s_hr_buf_idx >= HR_BUF_SIZE)   s_hr_buf_idx = 0;
    if (s_hr_buf_count > HR_BUF_SIZE)  s_hr_buf_count = HR_BUF_SIZE;
    if (s_trigger_streak > 0 && s_streak_start == 0) s_streak_start = time(NULL);
    if (s_hr_awake_baseline != 0 &&
        (s_hr_awake_baseline < BASELINE_MIN_BPM ||
         s_hr_awake_baseline > BASELINE_MAX_BPM)) {
        s_hr_awake_baseline = 0;  // re-seed from fresh data
    }
}

static void prv_reset_streak(void) {
    s_trigger_streak = 0;
    s_streak_start   = 0;
    persist_write_int(PERSIST_KEY_TRIGGER_STREAK, 0);
    persist_write_int(PERSIST_KEY_STREAK_START,   0);
}

// ─── HR Capability Probe ──────────────────────────────────────────────────────

/**
 * True if filtered HR data is usable on this watch. Uses a 1-hour lookback:
 * the firmware only implements HR accessibility queries for ranges within its
 * minute-data horizon (2 h), and a range this wide answers "does HR work here"
 * rather than "was there a sample in the last N seconds".
 * Returns false on basalt/chalk (no HRM) and when HR is disabled in settings.
 */
static bool prv_probe_hr_capable(void) {
    time_t now = time(NULL);
    HealthServiceAccessibilityMask mask =
        health_service_metric_accessible(HealthMetricHeartRateBPM,
                                         now - SECONDS_PER_HOUR, now);
    return (mask & HealthServiceAccessibilityMaskAvailable) != 0;
}

// ─── HealthService Subscribe / Unsubscribe / Boost ───────────────────────────

static void prv_health_event_handler(HealthEventType event, void *ctx);  // fwd

static void prv_subscribe_health(void) {
    if (s_health_subscribed) return;
    bool ok = health_service_events_subscribe(prv_health_event_handler, NULL);
    s_health_subscribed = ok;
    if (ok) {
        APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: HealthService subscribed");
    } else {
        APP_LOG(APP_LOG_LEVEL_WARNING,
            "NapBuster worker: HealthService unavailable");
    }
}

static void prv_unsubscribe_health(void) {
    if (!s_health_subscribed) return;
    health_service_events_unsubscribe();
    s_health_subscribed = false;
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: HealthService unsubscribed");
}

/**
 * Request (or cancel) a boosted HR sample period. Only active inside the
 * guard window — this is the one part of NapBuster that spends real battery,
 * and it's what makes ~2-minute detection cadence possible.
 */
static void prv_set_hr_boost(bool on) {
    if (!s_hr_capable) return;
    if (on == s_hr_boosted) return;
    bool ok = health_service_set_heart_rate_sample_period(
        on ? HR_BOOST_PERIOD_SECS : 0);
    s_hr_boosted = on && ok;
    APP_LOG(APP_LOG_LEVEL_INFO,
        "NapBuster worker: HR sample period %s (ok=%d)",
        on ? "boosted to 120s" : "reset to default", (int)ok);
}

// ─── Launch Logic ─────────────────────────────────────────────────────────────

/** Launch the foreground alarm if every guard passes. Returns true if launched. */
static bool prv_try_launch_foreground(void) {
    if (s_launch_pending)              return false;
    if (prv_is_already_alarming())     return false;
    if (prv_is_snoozed())              return false;
    if (prv_dismiss_cooldown_active()) return false;
    if (!prv_get_enabled())            return false;
    if (!prv_is_in_window())           return false;

    APP_LOG(APP_LOG_LEVEL_INFO,
        "NapBuster worker: sleep detected in no-nap window — launching app");

    // A pending nudge flag must not downgrade this launch to a double pulse
    persist_delete(PERSIST_KEY_NUDGE_PENDING);

    s_launch_pending = true;
    worker_launch_app();

    // If app is already in foreground, also send a direct message
    AppWorkerMessage msg = { .data0 = WORKER_MSG_SLEEP_DETECTED };
    app_worker_send_message(WORKER_MSG_SLEEP_DETECTED, &msg);
    return true;
}

/** Launch the foreground in nudge mode (double pulse, no alarm). */
static void prv_fire_nudge(void) {
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster Tier1: firing nudge");
    persist_write_int(PERSIST_KEY_NUDGE_PENDING, 1);
    worker_launch_app();
    AppWorkerMessage nudge_msg = { .data0 = WORKER_MSG_NAP_NUDGE };
    app_worker_send_message(WORKER_MSG_NAP_NUDGE, &nudge_msg);
}

// ─── Tier-1 Analysis (shared by HR event path and fallback timer) ─────────────

/**
 * Core Tier-1 analysis, called with a fresh HR reading (0 = unavailable).
 *
 * Requires BOTH a valid HR value and a valid VMC minute record; otherwise the
 * cycle is skipped and all detection state stays frozen. Resetting on missing
 * data was the v5 bug that made the full alarm nearly impossible to reach.
 */
static void prv_run_tier1_analysis(int16_t hr_val) {
    time_t now = time(NULL);

    // ── 1. Newest valid VMC from minute history ──────────────────────────────
    // Minute records can lag or be invalid; search back up to VMC_LOOKBACK_MIN.
    uint32_t current_vmc = 0;
    bool vmc_valid = false;
    HealthMinuteData minute_data[VMC_LOOKBACK_MIN];
    time_t start_time = now - (VMC_LOOKBACK_MIN * SECONDS_PER_MINUTE);
    time_t end_time   = now;
    uint32_t num_records = health_service_get_minute_history(
        minute_data, VMC_LOOKBACK_MIN, &start_time, &end_time);
    for (int i = (int)num_records - 1; i >= 0; i--) {
        if (!minute_data[i].is_invalid) {
            current_vmc = minute_data[i].vmc;
            vmc_valid = true;
            break;
        }
    }

    if (hr_val <= 0 || !vmc_valid) {
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: cycle skipped (hr=%d vmc_valid=%d) — state frozen",
            (int)hr_val, (int)vmc_valid);
        return;
    }

    // ── 2. Update VMC EMA ─────────────────────────────────────────────────────
    if (s_vmc_ema == 0) {
        s_vmc_ema = current_vmc;
    } else {
        s_vmc_ema = (s_vmc_ema * (VMC_EMA_ALPHA - 1) + current_vmc) / VMC_EMA_ALPHA;
    }

    // ── 3. Stillness: current minute OR the smoothed trend below threshold ───
    // The EMA term lets the streak survive a single restless minute mid-nap
    // (rolling over) without opening the gate during steady awake movement.
    bool still = (current_vmc < VMC_STILL_THRESH) ||
                 (s_vmc_ema   < VMC_STILL_THRESH);

    // ── 4. HR smoothing buffer ────────────────────────────────────────────────
    s_hr_buf[s_hr_buf_idx] = hr_val;
    s_hr_buf_idx = (s_hr_buf_idx + 1) % HR_BUF_SIZE;
    if (s_hr_buf_count < HR_BUF_SIZE) s_hr_buf_count++;

    int32_t sum = 0;
    for (uint8_t i = 0; i < s_hr_buf_count; i++) sum += s_hr_buf[i];
    int16_t smoothed_hr = (int16_t)(sum / s_hr_buf_count);

    // ── 5. Anchored awake baseline (asymmetric update) ────────────────────────
    // Up-moves are always safe (can't be sleep-onset chasing) unless we're
    // exercising, which would inflate the anchor. Down-moves need awake-zone
    // movement so the baseline never follows HR down into a nap.
    if (s_hr_awake_baseline == 0) {
        if (current_vmc >= VMC_BASELINE_MIN) {
            s_hr_awake_baseline = smoothed_hr;
            APP_LOG(APP_LOG_LEVEL_INFO,
                "NapBuster Tier1: awake baseline seeded at %d BPM (vmc=%u)",
                (int)s_hr_awake_baseline, (unsigned)current_vmc);
        }
    } else if (current_vmc >= VMC_BASELINE_MAX) {
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: baseline frozen (vmc=%u — exercising)",
            (unsigned)current_vmc);
    } else if (smoothed_hr >= s_hr_awake_baseline ||
               current_vmc >= VMC_BASELINE_MIN) {
        s_hr_awake_baseline =
            (int16_t)((s_hr_awake_baseline * 7 + smoothed_hr) / 8);
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: baseline -> %d (hr=%d vmc=%u)",
            (int)s_hr_awake_baseline, (int)smoothed_hr, (unsigned)current_vmc);
    } else {
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: baseline frozen (vmc=%u below %d and HR dropping)",
            (unsigned)current_vmc, VMC_BASELINE_MIN);
    }

    if (s_hr_awake_baseline != 0) {
        if (s_hr_awake_baseline < BASELINE_MIN_BPM) s_hr_awake_baseline = BASELINE_MIN_BPM;
        if (s_hr_awake_baseline > BASELINE_MAX_BPM) s_hr_awake_baseline = BASELINE_MAX_BPM;
    }

    // ── 6. Trigger evaluation ─────────────────────────────────────────────────
    if (s_hr_awake_baseline > 0 && s_hr_buf_count >= 2) {
        int drop_pct = prv_get_hr_drop_pct();
        bool hr_drop =
            ((int32_t)smoothed_hr * 100) < ((int32_t)s_hr_awake_baseline * drop_pct);

        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: hr=%d smoothed=%d base=%d vmc=%u still=%d drop=%d",
            (int)hr_val, (int)smoothed_hr, (int)s_hr_awake_baseline,
            (unsigned)current_vmc, (int)still, (int)hr_drop);

        if (hr_drop && still) {
            if (s_trigger_streak == 0) s_streak_start = now;
            if (s_trigger_streak < 255) s_trigger_streak++;
            int sustained = (int)(now - s_streak_start);

            APP_LOG(APP_LOG_LEVEL_INFO,
                "NapBuster Tier1: streak=%d sustained=%ds (nudge %ds, alarm %ds)",
                (int)s_trigger_streak, sustained, NUDGE_AFTER_SECS, ALARM_AFTER_SECS);

            if (s_trigger_streak >= ALARM_MIN_COUNT &&
                sustained >= ALARM_AFTER_SECS) {
                // Full alarm. Only consume the streak if the launch actually
                // happened — otherwise (e.g. outside window) keep accumulating
                // so a pre-window nap alarms the moment the window opens.
                if (prv_try_launch_foreground()) {
                    prv_reset_streak();
                }
            } else if (s_trigger_streak >= NUDGE_MIN_COUNT &&
                       sustained >= NUDGE_AFTER_SECS &&
                       (now - s_last_nudge_time) >= NUDGE_COOLDOWN_SECS &&
                       !s_launch_pending &&
                       !prv_is_already_alarming() &&
                       !prv_is_snoozed() &&
                       !prv_dismiss_cooldown_active() &&
                       prv_get_enabled() &&
                       prv_is_in_window()) {
                s_last_nudge_time = now;
                prv_fire_nudge();
            }
        } else {
            if (s_trigger_streak > 0) {
                APP_LOG(APP_LOG_LEVEL_INFO,
                    "NapBuster Tier1: streak reset by awake evidence (drop=%d still=%d)",
                    (int)hr_drop, (int)still);
            }
            s_trigger_streak = 0;
            s_streak_start   = 0;
        }
    } else {
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: warming up (base=%d buf=%d)",
            (int)s_hr_awake_baseline, (int)s_hr_buf_count);
    }

    // ── 7. Telemetry + persist ────────────────────────────────────────────────
    persist_write_int(PERSIST_KEY_DEBUG_HR,      (int)smoothed_hr);
    persist_write_int(PERSIST_KEY_DEBUG_AVG,     (int)s_hr_awake_baseline);
    persist_write_int(PERSIST_KEY_DEBUG_ACCEL,   (int)current_vmc);
    persist_write_int(PERSIST_KEY_DEBUG_LAST_TS, (int)now);
    prv_save_hr_state();
}

// ─── Tier-1 Fallback Sample Timer ────────────────────────────────────────────

static void prv_sample_timer_callback(void *ctx);  // fwd

static void prv_start_sample_timer(void) {
    if (s_sample_timer) return;
    s_sample_timer = app_timer_register(SAMPLE_INTERVAL_MS,
                                        prv_sample_timer_callback, NULL);
}

static void prv_stop_sample_timer(void) {
    if (s_sample_timer) {
        app_timer_cancel(s_sample_timer);
        s_sample_timer = NULL;
    }
}

/**
 * Fallback timer — fires every 5 minutes while the worker runs (HR platforms).
 * Skips entirely if a HealthEventHeartRateUpdate ran analysis recently, which
 * inside the window (boosted 120 s events) is essentially always. Outside the
 * window it keeps the awake baseline warm between sparse OS samples.
 * peek_current_value returns 0 for samples older than 15 min — a zero here
 * just skips the cycle, it no longer resets detection state.
 */
static void prv_sample_timer_callback(void *ctx) {
    s_sample_timer = NULL;

    time_t now = time(NULL);
    bool had_recent_event = (s_last_hr_event_time > 0) &&
                            (now - s_last_hr_event_time < SAMPLE_INTERVAL_SECS);

    if (had_recent_event) {
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1 timer: HR event ran analysis %ds ago — skipping",
            (int)(now - s_last_hr_event_time));
    } else {
        HealthValue peeked =
            health_service_peek_current_value(HealthMetricHeartRateBPM);
        int16_t hr_val = (peeked > 0) ? (int16_t)peeked : 0;
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1 timer: no recent HR event — peeked BPM=%d",
            (int)hr_val);
        prv_run_tier1_analysis(hr_val);
    }

    prv_start_sample_timer();  // re-arm
}

// ─── Window State Machine ─────────────────────────────────────────────────────

/**
 * Reconcile subscriptions, timers and the HR boost with the current window
 * state. Called at init, on every 60 s boundary tick, and on settings changes.
 */
static void prv_apply_window_state(void) {
    bool in_window = prv_get_enabled() && prv_is_in_window();
    bool was_active = s_window_active;
    s_window_active = in_window;

    // HR-capable platforms stay subscribed permanently so free piggybacked
    // events keep the awake baseline warm; Tier-2-only platforms subscribe
    // just for the window.
    if (s_hr_capable || in_window) {
        prv_subscribe_health();
    } else {
        prv_unsubscribe_health();
    }

    // Fallback timer mirrors HR capability, not the window
    if (s_hr_capable) {
        prv_start_sample_timer();
    } else {
        prv_stop_sample_timer();
    }

    // Boosted 120 s HR sampling only while guarding
    prv_set_hr_boost(in_window);

    if (in_window && !was_active) {
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker: window opened (baseline=%d, buf=%d)",
            (int)s_hr_awake_baseline, (int)s_hr_buf_count);

        // Immediate Tier-2 check in case we entered the window already asleep
        HealthActivityMask acts = health_service_peek_current_activities();
        if ((acts & HealthActivitySleep) || (acts & HealthActivityRestfulSleep)) {
            prv_try_launch_foreground();
        }
    } else if (!in_window && was_active) {
        APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: window closed");
        s_launch_pending = false;
        prv_reset_streak();
    }
}

// ─── Window Boundary Timer ────────────────────────────────────────────────────

static void prv_window_timer_callback(void *ctx);  // fwd

static void prv_start_window_timer(void) {
    if (s_window_timer) return;
    s_window_timer = app_timer_register(WINDOW_CHECK_MS,
                                        prv_window_timer_callback, NULL);
}

static void prv_window_timer_callback(void *ctx) {
    s_window_timer = NULL;

    // Self-heal HR capability: if the probe failed at worker start (e.g. right
    // after a reboot before the activity service warmed up), retry each minute.
    if (!s_hr_capable && prv_probe_hr_capable()) {
        s_hr_capable = true;
        prv_load_hr_state();
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker: HR became available — Tier 1 enabled");
    }

    prv_apply_window_state();
    prv_start_window_timer();  // re-arm
}

static void prv_stop_window_timer(void) {
    if (s_window_timer) {
        app_timer_cancel(s_window_timer);
        s_window_timer = NULL;
    }
}

// ─── HealthService Callback (Tier 1 fast path + Tier 2 fallback) ─────────────

static void prv_health_event_handler(HealthEventType event, void *ctx) {
    // ── HealthEventSignificantUpdate — re-check sleep state ──────────────────
    if (event == HealthEventSignificantUpdate) {
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster: HealthEventSignificantUpdate — re-checking sleep");
        HealthActivityMask activities = health_service_peek_current_activities();
        bool is_sleeping = (activities & HealthActivitySleep) ||
                           (activities & HealthActivityRestfulSleep);
        if (is_sleeping) {
            prv_try_launch_foreground();
        } else {
            s_launch_pending = false;
        }
        return;
    }

    if (event == HealthEventHeartRateUpdate && s_hr_capable) {
        // ── Tier 1 fast path — new HR reading available ───────────────────────
        // peek self-limits to samples <15 min old; a 0 just skips the cycle.
        HealthValue peeked =
            health_service_peek_current_value(HealthMetricHeartRateBPM);
        int16_t hr_val = (peeked > 0) ? (int16_t)peeked : 0;
        s_last_hr_event_time = time(NULL);
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: HR event — BPM=%d", (int)hr_val);
        prv_run_tier1_analysis(hr_val);
        return;
    }

    // ── Tier 2 — sleep confirmation via OS activity classification ───────────
    HealthActivityMask activities = health_service_peek_current_activities();
    bool is_sleeping = (activities & HealthActivitySleep) ||
                       (activities & HealthActivityRestfulSleep);

    if (is_sleeping) {
        APP_LOG(APP_LOG_LEVEL_INFO,
            s_hr_capable ? "NapBuster Tier2 (fallback): sleep confirmed"
                         : "NapBuster Tier2: sleep confirmed");
        prv_try_launch_foreground();
    } else {
        // User is awake — reset so a future doze-off can re-trigger
        s_launch_pending = false;
    }
}

// ─── App Message Handler ─────────────────────────────────────────────────────

static void prv_app_message_handler(uint16_t type, AppWorkerMessage *msg) {
    switch (type) {
        case APP_MSG_DISMISS:
        case APP_MSG_SNOOZE_10:
        case APP_MSG_SNOOZE_30:
            s_launch_pending = false;
            prv_reset_streak();
            APP_LOG(APP_LOG_LEVEL_INFO,
                "NapBuster worker: alarm %s acknowledged",
                type == APP_MSG_DISMISS ? "dismiss" : "snooze");
            break;

        case APP_MSG_SETTINGS_CHANGED:
            s_launch_pending = false;
            prv_reset_streak();
            prv_apply_window_state();
            APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: settings reloaded");
            break;

        default:
            break;
    }
}

// ─── Worker Lifecycle ─────────────────────────────────────────────────────────

static void worker_init(void) {
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker v6: starting");

    app_worker_message_subscribe(prv_app_message_handler);

    // Runtime HR capability detection (self-heals in the window timer if the
    // health service isn't ready yet). False on basalt/chalk → Tier 2 only.
    s_hr_capable = prv_probe_hr_capable();
    APP_LOG(APP_LOG_LEVEL_INFO,
        "NapBuster worker v6: HR capable=%d", (int)s_hr_capable);

    if (s_hr_capable) {
        prv_load_hr_state();
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker v6: loaded HR state count=%d streak=%d baseline=%d",
            (int)s_hr_buf_count, (int)s_trigger_streak, (int)s_hr_awake_baseline);
    }

    // Reconcile subscriptions / boost / timers with the current window state.
    // s_window_active starts false, so starting inside the window runs the
    // "window opened" path including the immediate Tier-2 sleep check.
    prv_apply_window_state();

    // Always run the 60 s boundary timer
    prv_start_window_timer();
}

static void worker_deinit(void) {
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker v6: stopping");

    prv_set_hr_boost(false);  // never leave a boosted sample period behind
    prv_stop_sample_timer();
    prv_stop_window_timer();
    prv_unsubscribe_health();
    app_worker_message_unsubscribe();

    if (s_hr_capable) {
        prv_save_hr_state();
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker v6: saved HR state count=%d streak=%d",
            (int)s_hr_buf_count, (int)s_trigger_streak);
    }
}

// ─── Entry Point ─────────────────────────────────────────────────────────────

int main(void) {
    worker_init();
    worker_event_loop();
    worker_deinit();
    return 0;
}
