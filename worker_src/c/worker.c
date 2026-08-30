/**
 * worker.c — NapBuster Background Worker v8
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
 *   HRV (SDK 4.33+, Pebble Time 2 hardware): alongside the HR period the
 *   worker requests an HRV sample period at the SAME 120 s interval — the two
 *   share one sensor subscription driven at the shorter period, so this adds
 *   no sensor wakeups over v2.2. The Goodix driver reports up to 4
 *   adjacent-beat RR intervals per sensor burst, one HealthEventHRVUpdate
 *   each, with NO firmware quality filtering — so we artifact-gate each PPI
 *   (physiological range + agreement with current HR), group readings into
 *   bursts (≤ BURST_GAP_SECS apart), and keep a ring of burst-mean PPIs.
 *   The mean absolute successive difference over that ring ("drift spread")
 *   measures how much average HR wanders between bursts — HIGH while awake
 *   (on-wrist: ~140-165 ms), COLLAPSING as the user dozes (~30 ms). v2.3.0
 *   assumed textbook rMSSD behavior (variability rises at sleep onset) and
 *   had this inverted; the drift term dominates at a 2-minute cadence and
 *   moves the other way. Detection is HRV-primary where available:
 *     positive = still AND (drift suppressed AND HR mildly low)  ← early path
 *                       OR (HR fully dropped)                    ← v2.2 path
 *   The awake-drift baseline anchors with DOWN as the dangerous direction:
 *   it only falls while HR proves wakefulness, and rises only with a quiet
 *   wrist (motion garbage must not inflate it). Within-burst adjacent-beat
 *   variability (true rMSSD material, expected to RISE in sleep) is computed
 *   and logged per burst for future validation, but does not yet gate.
 *   On watches without HRV (Pebble 2, or firmware <4.33) the request fails
 *   and detection is exactly the v2.2 HR-only behavior.
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
 *   HR-capable  → HealthService subscribed during the guard window AND for a
 *                 WARM_LEAD_HOURS (2h) lead-in beforehand, so the awake
 *                 baseline is warm the moment guarding starts. Fully
 *                 unsubscribed the rest of the day — no more 24/7 always-on
 *                 subscription. 5-min fallback timer only runs while
 *                 subscribed. HR boost only inside the window itself.
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
#define PERSIST_KEY_HRV_BASELINE      24  // int16: anchored awake drift baseline (ms)
#define PERSIST_KEY_DEBUG_HRV         25  // int: last drift spread (ms), -1 = unavailable

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
#define WARM_LEAD_HOURS       2       // subscribe/analyze this long before window open too

// ─── HRV (PPI) Constants — SDK 4.33+, Pebble Time 2 hardware ────────────────
// HRV shares the HR sensor subscription (driven at the shorter period), so an
// equal period means the sensor fires no more often than the HR boost alone.
#define HRV_PERIOD_SECS       120     // in-window HRV sample period request
#define BURST_GAP_SECS        10      // PPIs closer together than this = one sensor burst
#define HRV_BUF_SIZE          8       // accepted-PPI ring (~16 min at 120 s)
#define HRV_MIN_SAMPLES       5       // spread needs at least this many PPIs
#define HRV_STALE_SECS        900     // ring unusable if newest PPI older than this
#define PPI_MIN_MS            300     // artifact gate: >200 bpm is not a resting beat
#define PPI_MAX_MS            2000    // artifact gate: <30 bpm is not a real beat
#define PPI_HR_TOLERANCE_PCT  40      // reject PPI >±40% off the HR-implied interval
#define HRV_BASELINE_MIN_MS   5       // sanity clamp for the awake-spread baseline
#define HRV_BASELINE_MAX_MS   200
#define AWAKE_HR_PCT          95      // smoothed HR ≥95% of baseline = clearly awake
#define HRV_QUIET_VMC         200     // PPIs only trustworthy with a reasonably quiet wrist

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

// ─── HRV state ────────────────────────────────────────────────────────────────

// True once an HRV sample-period request has succeeded (PT2 on SDK 4.33+).
// Stays false on hardware/firmware without HRV → pure HR-only detection.
static bool     s_hrv_capable = false;

// True while our HRV sample period request is active
static bool     s_hrv_boosted = false;

// True once an HRV request was definitively rejected (no HRV hardware or
// firmware <4.33) — stops per-minute retry/log spam. Cleared only by worker
// restart, which is when a firmware update could have changed the answer.
static bool     s_hrv_rejected = false;

// The Goodix driver reports up to 4 adjacent-beat RR intervals per sensor
// burst, each as its own HealthEventHRVUpdate. Accepted PPIs are accumulated
// per burst; the ring stores one MEAN PPI per completed burst, so the spread
// over the ring is a pure inter-burst drift metric (how much average HR
// wanders between samples) — high awake, collapsing during sleep.
static uint16_t s_burst_buf[HRV_BUF_SIZE];   // burst-mean PPIs, ms
static uint8_t  s_burst_idx   = 0;
static uint8_t  s_burst_count = 0;
static time_t   s_last_burst_time = 0;

// In-progress burst accumulator (finalized when the next volley starts, or
// when an analysis cycle finds it older than BURST_GAP_SECS)
static uint32_t s_cur_burst_sum      = 0;
static uint8_t  s_cur_burst_n        = 0;
static time_t   s_cur_burst_start    = 0;
static uint16_t s_cur_burst_prev_ppi = 0;
static uint32_t s_cur_burst_rsa_sum  = 0;   // |diff| of adjacent-beat RRs
static uint8_t  s_cur_burst_rsa_n    = 0;   // (true rMSSD material — logged only)

// Anchored awake drift baseline (ms). Sleep onset LOWERS drift, so down-moves
// are gated on proof of wakefulness and up-moves need only a quiet wrist
// (motion-garbage PPIs must not inflate the anchor). 0 = not yet established.
static int16_t  s_hrv_awake_baseline = 0;

// Last smoothed HR, used to artifact-gate incoming PPIs against the
// HR-implied beat interval (60000/bpm).
static int16_t  s_last_smoothed_hr = 0;

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

/**
 * Return the HR-drop threshold percentage based on persisted sensitivity.
 *
 * Bumped up (bigger required drop) from the original 8/13/20% — normal
 * resting relaxation (sitting/lying still, reading, watching TV) commonly
 * drops HR 10-15% below an "active" baseline via vagal tone without being
 * sleep onset. The old thresholds were catching that as a nap.
 */
static int prv_get_hr_drop_pct(void) {
    int level = persist_exists(PERSIST_KEY_SENSITIVITY)
                ? persist_read_int(PERSIST_KEY_SENSITIVITY)
                : DEFAULT_SENSITIVITY;
    switch (level) {
        case 0: return 90;  // Sensitive:     10% drop
        case 2: return 76;  // Conservative: 24% drop
        default: return 84; // Balanced:     16% drop
    }
}

/**
 * Softer HR threshold used when HRV corroborates: halfway between "no drop"
 * and the full sensitivity drop. Drift suppression appears earlier in the
 * doze-off slide than a full HR drop, so with drift suppressed we accept a
 * milder HR dip. Sensitive: 95, Balanced: 92, Conservative: 88.
 */
static int prv_get_hr_soft_pct(void) {
    return (100 + prv_get_hr_drop_pct()) / 2;
}

/**
 * Drift-suppression threshold (percent of the awake baseline) for the HRV
 * path: positive when spread ≤ baseline × pct / 100. On-wrist data (PT2):
 * quiet-awake drift ≈ 140–165 ms, dozing/napping ≈ 30 ms — sleep stabilizes
 * average HR, so inter-burst drift collapses. Thresholds leave wide margin.
 */
static int prv_get_hrv_suppress_pct(void) {
    int level = persist_exists(PERSIST_KEY_SENSITIVITY)
                ? persist_read_int(PERSIST_KEY_SENSITIVITY)
                : DEFAULT_SENSITIVITY;
    switch (level) {
        case 0: return 60;  // Sensitive:    ≤60% of awake drift
        case 2: return 40;  // Conservative: ≤40%
        default: return 50; // Balanced:     ≤50%
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

/**
 * True during the WARM_LEAD_HOURS window immediately before the guard window
 * opens, on an active day. Lets HR-capable platforms subscribe/analyze early
 * so the awake baseline is already warm the moment guarding actually starts,
 * without needing a 24/7 subscription the rest of the day.
 */
static bool prv_is_in_lead_period(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    uint8_t active_days = persist_exists(PERSIST_KEY_ACTIVE_DAYS)
        ? (uint8_t)persist_read_int(PERSIST_KEY_ACTIVE_DAYS)
        : DEFAULT_ACTIVE_DAYS;
    active_days &= 0x7F;
    if (active_days == 0) active_days = DEFAULT_ACTIVE_DAYS;
    if (!((active_days >> t->tm_wday) & 1)) return false;

    int hour       = t->tm_hour;
    int start      = prv_get_start_hour();
    int lead_start = ((start - WARM_LEAD_HOURS) % 24 + 24) % 24;

    if (lead_start <= start) {
        return (hour >= lead_start && hour < start);
    } else {
        return (hour >= lead_start || hour < start);
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
    persist_write_int(PERSIST_KEY_HRV_BASELINE,    (int)s_hrv_awake_baseline);
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
    s_hrv_awake_baseline = persist_exists(PERSIST_KEY_HRV_BASELINE)
                           ? (int16_t)persist_read_int(PERSIST_KEY_HRV_BASELINE) : 0;

    // Guard restored values
    if (s_hr_buf_idx >= HR_BUF_SIZE)   s_hr_buf_idx = 0;
    if (s_hr_buf_count > HR_BUF_SIZE)  s_hr_buf_count = HR_BUF_SIZE;
    if (s_trigger_streak > 0 && s_streak_start == 0) s_streak_start = time(NULL);
    if (s_hr_awake_baseline != 0 &&
        (s_hr_awake_baseline < BASELINE_MIN_BPM ||
         s_hr_awake_baseline > BASELINE_MAX_BPM)) {
        s_hr_awake_baseline = 0;  // re-seed from fresh data
    }
    if (s_hrv_awake_baseline != 0 &&
        (s_hrv_awake_baseline < HRV_BASELINE_MIN_MS ||
         s_hrv_awake_baseline > HRV_BASELINE_MAX_MS)) {
        s_hrv_awake_baseline = 0;  // re-seed from fresh data
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

/**
 * Request (or cancel) the HRV (PPI) sample period. Shares the sensor
 * subscription with the HR period at the shorter of the two intervals —
 * requesting both at 120 s costs no sensor wakeups over the HR boost alone.
 * The request fails on hardware/firmware without HRV (Pebble 2, PebbleOS
 * <4.33); s_hrv_capable latches so detection knows which mode it's in.
 */
static void prv_set_hrv_boost(bool on) {
    if (!s_hr_capable) return;
    if (on && s_hrv_rejected) return;  // hardware said no — stop asking
    if (on == s_hrv_boosted) return;
    bool ok = health_service_set_hrv_sample_period(on ? HRV_PERIOD_SECS : 0);
    s_hrv_boosted = on && ok;
    if (on) {
        s_hrv_capable = ok;
        s_hrv_rejected = !ok;
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker: HRV sample period request %s",
            ok ? "accepted — HRV-primary detection" : "rejected — HR-only detection");
    } else {
        APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: HRV sample period reset");
    }
}

// ─── HRV: burst-aware PPI ingest + drift spread ──────────────────────────────
//
// The Goodix driver delivers up to 4 adjacent-beat RR intervals per sensor
// burst, one HealthEventHRVUpdate each. Flattening those into a single ring
// (v2.3.0) mixed two different signals: within-burst variability (respiratory,
// rises in sleep) and inter-burst HR drift (falls in sleep). The drift term
// dominates a successive-difference stat, which is why the v2.3.0 "spread"
// empirically DROPPED during naps. v2.4.0 separates them: each burst reduces
// to its mean PPI, the ring holds burst means, and the spread over the ring is
// a clean drift metric. Within-burst variability is logged for future use.

/** Push the completed in-progress burst (if any) into the burst ring. */
static void prv_finalize_burst(void) {
    if (s_cur_burst_n == 0) return;

    uint16_t mean = (uint16_t)(s_cur_burst_sum / s_cur_burst_n);
    s_burst_buf[s_burst_idx] = mean;
    s_burst_idx = (s_burst_idx + 1) % HRV_BUF_SIZE;
    if (s_burst_count < HRV_BUF_SIZE) s_burst_count++;
    s_last_burst_time = time(NULL);

    if (s_cur_burst_rsa_n > 0) {
        // Adjacent-beat variability inside the burst — true rMSSD material
        // (rises at sleep onset). Telemetry-only until validated on-wrist.
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster HRV: burst mean=%u ms n=%u rsa=%lu ms (%d bursts)",
            (unsigned)mean, (unsigned)s_cur_burst_n,
            (unsigned long)(s_cur_burst_rsa_sum / s_cur_burst_rsa_n),
            (int)s_burst_count);
    } else {
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster HRV: burst mean=%u ms n=%u (%d bursts)",
            (unsigned)mean, (unsigned)s_cur_burst_n, (int)s_burst_count);
    }

    s_cur_burst_sum      = 0;
    s_cur_burst_n        = 0;
    s_cur_burst_start    = 0;
    s_cur_burst_prev_ppi = 0;
    s_cur_burst_rsa_sum  = 0;
    s_cur_burst_rsa_n    = 0;
}

/**
 * Artifact-gate one raw PPI reading and accumulate it into the current burst.
 * The firmware forwards every driver reading with NO quality filtering
 * (quality exists internally but is not exposed), so this gate is our only
 * defense against motion artifacts, poor contact, and off-wrist noise:
 *   1. physiological range 300–2000 ms (30–200 bpm), and
 *   2. within ±PPI_HR_TOLERANCE_PCT of the interval implied by current HR —
 *      wide enough for genuine beat-to-beat variability, tight enough to
 *      reject sensor garbage.
 */
static void prv_ingest_ppi(uint16_t ppi_ms) {
    if (ppi_ms < PPI_MIN_MS || ppi_ms > PPI_MAX_MS) {
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster HRV: PPI %u ms rejected (out of range)", (unsigned)ppi_ms);
        return;
    }
    if (s_last_smoothed_hr > 0) {
        int32_t expected = 60000 / s_last_smoothed_hr;
        int32_t dev = (int32_t)ppi_ms - expected;
        if (dev < 0) dev = -dev;
        if (dev * 100 > expected * PPI_HR_TOLERANCE_PCT) {
            APP_LOG(APP_LOG_LEVEL_DEBUG,
                "NapBuster HRV: PPI %u ms rejected (HR-implied %ld ms)",
                (unsigned)ppi_ms, (long)expected);
            return;
        }
    }

    time_t now = time(NULL);

    // A gap since the burst started means the previous volley is complete
    if (s_cur_burst_n > 0 && (now - s_cur_burst_start) > BURST_GAP_SECS) {
        prv_finalize_burst();
    }

    if (s_cur_burst_n == 0) {
        s_cur_burst_start = now;
    } else {
        int32_t d = (int32_t)ppi_ms - (int32_t)s_cur_burst_prev_ppi;
        if (d < 0) d = -d;
        s_cur_burst_rsa_sum += (uint32_t)d;
        s_cur_burst_rsa_n++;
    }
    s_cur_burst_sum += ppi_ms;
    s_cur_burst_n++;
    s_cur_burst_prev_ppi = ppi_ms;
}

/**
 * Drift spread: mean absolute successive difference (ms) over the burst-mean
 * ring in chronological order — how much average HR wanders between sensor
 * bursts. High while awake (posture, cognition, micro-arousals keep HR
 * moving); collapses during sleep. Returns -1 when there aren't enough fresh
 * bursts. Finalizes a lingering in-progress burst first so analysis cycles
 * always see the newest complete volley.
 */
static int prv_compute_ppi_spread(void) {
    if (s_cur_burst_n > 0 &&
        (time(NULL) - s_cur_burst_start) > BURST_GAP_SECS) {
        prv_finalize_burst();
    }

    if (s_burst_count < HRV_MIN_SAMPLES) return -1;
    if (s_last_burst_time == 0 ||
        (time(NULL) - s_last_burst_time) > HRV_STALE_SECS) return -1;

    uint8_t start = (s_burst_count == HRV_BUF_SIZE) ? s_burst_idx : 0;
    int32_t sum = 0;
    for (uint8_t i = 1; i < s_burst_count; i++) {
        int32_t a = s_burst_buf[(uint8_t)((start + i - 1) % HRV_BUF_SIZE)];
        int32_t b = s_burst_buf[(uint8_t)((start + i) % HRV_BUF_SIZE)];
        int32_t d = b - a;
        if (d < 0) d = -d;
        sum += d;
    }
    return (int)(sum / (s_burst_count - 1));
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
    s_last_smoothed_hr = smoothed_hr;  // used to artifact-gate incoming PPIs

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

    // ── 5b. Drift spread + anchored awake-drift baseline ──────────────────────
    // Sleep onset LOWERS drift, so DOWN is the dangerous direction: down-moves
    // only happen while HR proves wakefulness (v2.3.0 had this mirrored, and
    // the baseline chased the user into their doze). Up-moves are safe from
    // sleep-chasing but need a quiet wrist so motion-garbage PPIs can't
    // inflate the anchor.
    int spread = prv_compute_ppi_spread();
    if (spread >= 0 && s_hr_awake_baseline > 0) {
        bool clearly_awake =
            ((int32_t)smoothed_hr * 100 >=
             (int32_t)s_hr_awake_baseline * AWAKE_HR_PCT) &&
            (current_vmc < HRV_QUIET_VMC);

        if (s_hrv_awake_baseline == 0) {
            if (clearly_awake) {
                s_hrv_awake_baseline = (int16_t)spread;
                APP_LOG(APP_LOG_LEVEL_INFO,
                    "NapBuster HRV: awake drift baseline seeded at %d ms", spread);
            }
        } else if ((spread >= s_hrv_awake_baseline &&
                    current_vmc < HRV_QUIET_VMC) ||
                   clearly_awake) {
            s_hrv_awake_baseline =
                (int16_t)((s_hrv_awake_baseline * 7 + spread) / 8);
        }

        if (s_hrv_awake_baseline != 0) {
            if (s_hrv_awake_baseline < HRV_BASELINE_MIN_MS)
                s_hrv_awake_baseline = HRV_BASELINE_MIN_MS;
            if (s_hrv_awake_baseline > HRV_BASELINE_MAX_MS)
                s_hrv_awake_baseline = HRV_BASELINE_MAX_MS;
        }
    }

    // ── 6. Trigger evaluation ─────────────────────────────────────────────────
    if (s_hr_awake_baseline > 0 && s_hr_buf_count >= 2) {
        int drop_pct = prv_get_hr_drop_pct();
        bool hr_drop_full =
            ((int32_t)smoothed_hr * 100) < ((int32_t)s_hr_awake_baseline * drop_pct);

        // HRV-primary path: suppressed inter-burst drift (average HR gone
        // metronomic — the earliest observable sign of the doze-off slide)
        // plus a mild HR dip fires earlier than the full HR drop. The
        // full-drop path stays live as insurance for naps where the drift
        // signal is missing or the baseline is still warming up.
        bool hrv_ready = (spread >= 0 && s_hrv_awake_baseline > 0);
        bool hrv_positive = false;
        if (hrv_ready) {
            bool hrv_suppressed =
                (int32_t)spread * 100 <=
                (int32_t)s_hrv_awake_baseline * prv_get_hrv_suppress_pct();
            bool hr_soft_drop =
                ((int32_t)smoothed_hr * 100) <
                ((int32_t)s_hr_awake_baseline * prv_get_hr_soft_pct());
            hrv_positive = hrv_suppressed && hr_soft_drop;
        }

        bool positive = still && (hrv_positive || hr_drop_full);

        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: hr=%d smoothed=%d base=%d vmc=%u still=%d "
            "drop=%d spread=%d hrvbase=%d hrv+=%d",
            (int)hr_val, (int)smoothed_hr, (int)s_hr_awake_baseline,
            (unsigned)current_vmc, (int)still, (int)hr_drop_full,
            spread, (int)s_hrv_awake_baseline, (int)hrv_positive);

        if (positive) {
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
                    "NapBuster Tier1: streak reset by awake evidence (drop=%d hrv+=%d still=%d)",
                    (int)hr_drop_full, (int)hrv_positive, (int)still);
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
    persist_write_int(PERSIST_KEY_DEBUG_HRV,     spread);
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
 * Fallback timer — fires every 5 minutes while subscribed (guard window or
 * the WARM_LEAD_HOURS lead-in), stopped entirely the rest of the day.
 * Skips entirely if a HealthEventHeartRateUpdate ran analysis recently, which
 * inside the window (boosted 120 s events) is essentially always.
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
    bool enabled    = prv_get_enabled();
    bool in_window  = enabled && prv_is_in_window();
    bool was_active = s_window_active;
    s_window_active = in_window;

    // Subscribe during the guard window itself, plus a WARM_LEAD_HOURS lead-in
    // beforehand on HR-capable platforms (so the awake baseline is already
    // warm the moment guarding starts). Outside both, fully unsubscribe — no
    // more 24/7 always-on subscription burning battery all day.
    bool in_lead      = enabled && s_hr_capable && !in_window && prv_is_in_lead_period();
    bool want_health  = in_window || in_lead;

    if (want_health) {
        prv_subscribe_health();
    } else {
        prv_unsubscribe_health();
    }

    // Fallback timer only needed while actually subscribed for Tier 1
    // (window or lead-in) — no reason to wake every 5 min the rest of the day.
    if (s_hr_capable && want_health) {
        prv_start_sample_timer();
    } else {
        prv_stop_sample_timer();
    }

    // Boosted 120 s HR sampling only while actually guarding.
    // HRV rides the same sensor subscription at the same period — no extra
    // sensor wakeups — and is likewise only requested while guarding.
    prv_set_hr_boost(in_window);
    prv_set_hrv_boost(in_window);

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

    if (event == HealthEventHRVUpdate) {
        // ── HRV fast path — one raw PPI reading per event ─────────────────────
        // Events only arrive while we hold an HRV sample period; the reading is
        // fresh by construction. Ingest (artifact-gated) and return — the
        // spread is evaluated on the next analysis cycle alongside HR + VMC.
        uint16_t ppi = health_service_peek_hrv_ppi_ms();
        if (ppi > 0) prv_ingest_ppi(ppi);
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
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker v8: starting");

    app_worker_message_subscribe(prv_app_message_handler);

    // Runtime HR capability detection (self-heals in the window timer if the
    // health service isn't ready yet). False on basalt/chalk → Tier 2 only.
    s_hr_capable = prv_probe_hr_capable();
    APP_LOG(APP_LOG_LEVEL_INFO,
        "NapBuster worker v8: HR capable=%d", (int)s_hr_capable);

    if (s_hr_capable) {
        prv_load_hr_state();
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker v8: loaded HR state count=%d streak=%d baseline=%d",
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
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker v8: stopping");

    prv_set_hr_boost(false);   // never leave a boosted sample period behind
    prv_set_hrv_boost(false);  // ditto for the HRV period
    prv_stop_sample_timer();
    prv_stop_window_timer();
    prv_unsubscribe_health();
    app_worker_message_unsubscribe();

    if (s_hr_capable) {
        prv_save_hr_state();
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker v8: saved HR state count=%d streak=%d",
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
