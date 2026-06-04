/**
 * worker.c — NapBuster Background Worker v5
 *
 * Two-tier sleep detection:
 *
 *   TIER 1 (HR-capable platforms: emery/diorite)
 *   ─────────────────────────────────────────────
 *   Piggybacks on HealthEventHeartRateUpdate (free — OS was already waking for HR):
 *     • On every HR event: read HR + VMC, run analysis immediately
 *     • 5-min fallback timer: only runs analysis if no HR event arrived recently
 *       (handles users with slow/disabled background HR sampling setting)
 *     • Maintain anchored awake HR baseline (only updates upward when moving)
 *     • 3-sample smoothing buffer for current HR (noise reduction)
 *     • If smoothed HR drops below awake baseline by threshold AND VMC still
 *       for ≥2 consecutive detections → alarm. Baseline never drifts down
 *       with sleep onset — fixes gradual-drop detection failure.
 *
 *   TIER 2 (ALL platforms including HR-capable)
 *   ─────────────────────────────────────────────
 *   HealthService sleep event subscription runs alongside Tier 1 as a fallback.
 *   On platforms without HR (basalt/chalk), health_service_peek_current_value
 *   returns 0 → s_hr_capable=false → only Tier 2 runs (graceful degradation,
 *   no compile-time ifdefs needed).
 *
 *   OUTSIDE window → HealthService UNSUBSCRIBED, sample timer stopped
 *                    A 60-second timer polls the clock to detect window open
 *
 *   INSIDE window  → HealthService SUBSCRIBED (Tier 2)
 *                    5-min sample timer running if hr_capable (Tier 1)
 *
 * SDK notes:
 *   - worker_launch_app() returns void
 *   - WakeupId / wakeup_schedule() NOT available in worker context
 *   - HealthActivitySleep (NOT HealthActivityMaskSleep) is the correct enum
 *   - health_service_get_minute_history() provides VMC + HR without extra sensor power
 *   - health_service_peek_current_value returns HealthValue (int32_t)
 *   - health_service_metric_accessible() guards against unavailable HR data
 */

#include <pebble_worker.h>

// ─── Shared Persist Keys ──────────────────────────────────────────────────────

#define PERSIST_KEY_ENABLED            0
#define PERSIST_KEY_START_HOUR         1
#define PERSIST_KEY_END_HOUR           2
#define PERSIST_KEY_SNOOZE_UNTIL       3
#define PERSIST_KEY_ALARMING           4
#define PERSIST_KEY_WEEKDAYS_ONLY      8  // DEPRECATED — replaced by ACTIVE_DAYS
#define PERSIST_KEY_ACTIVE_DAYS        8  // uint8 bitmask bit0=Sun..bit6=Sat

// New Tier-1 state (persisted across worker restarts)
#define PERSIST_KEY_HR_BUFFER         10  // int16_t[8] blob
#define PERSIST_KEY_HR_BUF_IDX        11  // uint8_t write index
#define PERSIST_KEY_HR_BUF_COUNT      12  // uint8_t valid count (max HR_BUF_SIZE)
#define PERSIST_KEY_TRIGGER_STREAK    13  // uint8_t consecutive trigger count
#define PERSIST_KEY_VMC_EMA           14  // uint32_t EMA of VMC (Vector Magnitude Count)

// Tier-1 debug telemetry (written each analysis cycle, read by foreground app)
#define PERSIST_KEY_DEBUG_HR          15  // int16: last sampled HR BPM
#define PERSIST_KEY_DEBUG_AVG         16  // int16: rolling HR average
#define PERSIST_KEY_DEBUG_ACCEL       17  // int32: last VMC reading

// Detection sensitivity (shared with foreground app)
#define PERSIST_KEY_SENSITIVITY       18  // int: 0=Sensitive 1=Balanced 2=Conservative
#define DEFAULT_SENSITIVITY           1   // Balanced

#define PERSIST_KEY_HR_BASELINE      19  // int16: anchored awake HR baseline
#define PERSIST_KEY_NUDGE_PENDING    20  // bool: worker set nudge, foreground should pulse+clear

// ─── Defaults ─────────────────────────────────────────────────────────────────

#define DEFAULT_ENABLED                1
#define DEFAULT_START_HOUR             11
#define DEFAULT_END_HOUR               23
#define DEFAULT_ACTIVE_DAYS            0x7F  // every day

// ─── Messages ─────────────────────────────────────────────────────────────────

#define WORKER_MSG_SLEEP_DETECTED      0
#define WORKER_MSG_NAP_NUDGE           1  // x1 streak: gentle nudge request
#define APP_MSG_SNOOZE_10              10
#define APP_MSG_SNOOZE_30              11
#define APP_MSG_DISMISS                12
#define APP_MSG_SETTINGS_CHANGED       13

// ─── Tier-1 Constants ─────────────────────────────────────────────────────────

#define HR_BUF_SIZE          3       // short smoothing buffer (3 readings ~= 30min at 10-min sampling)
#define HR_DROP_PCT          87      // trigger if hr*100 < rolling_avg * HR_DROP_PCT
#define VMC_STILL_THRESH     100     // VMC below this = too still — possible sleep, freeze baseline
#define VMC_BASELINE_MIN     50      // VMC must be above this to update baseline (not asleep/dozing)
#define VMC_BASELINE_MAX     400     // VMC must be below this to update baseline (not exercising)
#define VMC_EMA_ALPHA        4       // EMA weight: faster than old accel EMA (VMC is per-minute)
#define STREAK_TO_FIRE       2       // consecutive detections before alarm
#define SAMPLE_INTERVAL_MS   300000  // 5 minutes in ms (fallback timer)
#define SAMPLE_INTERVAL_SECS 300     // same in seconds (for recency check)
#define WINDOW_CHECK_MS      60000   // 1 minute window boundary check

// ─── State ────────────────────────────────────────────────────────────────────

// True once worker_launch_app() called and haven't gotten a dismiss yet
static bool s_launch_pending    = false;

// True when HealthService is currently subscribed
static bool s_health_subscribed = false;

// True if this platform has a working HR sensor (runtime-detected)
static bool s_hr_capable        = false;

// 60-second window boundary timer
static AppTimer *s_window_timer  = NULL;

// 5-minute Tier-1 sample timer (only used if s_hr_capable)
static AppTimer *s_sample_timer  = NULL;

// Tier-1 HR circular buffer and metadata
static int16_t  s_hr_buf[HR_BUF_SIZE];
static uint8_t  s_hr_buf_idx   = 0;   // next write position
static uint8_t  s_hr_buf_count = 0;   // number of valid entries (0..HR_BUF_SIZE)

// Tier-1 consecutive detection streak
static uint8_t  s_trigger_streak = 0;

// Tier-1 VMC EMA (Vector Magnitude Count — pre-computed motion from HealthMinuteData)
static uint32_t s_vmc_ema       = 0;

// Anchored awake HR baseline — only updates upward when VMC is high (clearly moving).
// Never drifts down as user falls asleep. This is the stable reference for hr_drop.
static int16_t  s_hr_awake_baseline = 0;  // 0 = not yet established

// Timestamp of last HealthEventHeartRateUpdate (0 = never)
// Used by the fallback timer to avoid duplicate analysis
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

    // Active-days bitmask check (bit 0=Sun ... bit 6=Sat)
    uint8_t active_days = persist_exists(PERSIST_KEY_ACTIVE_DAYS)
        ? (uint8_t)persist_read_int(PERSIST_KEY_ACTIVE_DAYS)
        : DEFAULT_ACTIVE_DAYS;
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

// ─── Tier-1 Persist Helpers ──────────────────────────────────────────────────

static void prv_save_hr_state(void) {
    persist_write_data(PERSIST_KEY_HR_BUFFER, s_hr_buf,
                       sizeof(int16_t) * HR_BUF_SIZE);
    persist_write_int(PERSIST_KEY_HR_BUF_IDX,      s_hr_buf_idx);
    persist_write_int(PERSIST_KEY_HR_BUF_COUNT,    s_hr_buf_count);
    persist_write_int(PERSIST_KEY_TRIGGER_STREAK,  s_trigger_streak);
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
    s_vmc_ema         = persist_exists(PERSIST_KEY_VMC_EMA)
                        ? (uint32_t)persist_read_int(PERSIST_KEY_VMC_EMA)       : 0;
    s_hr_awake_baseline = persist_exists(PERSIST_KEY_HR_BASELINE)
                          ? (int16_t)persist_read_int(PERSIST_KEY_HR_BASELINE) : 0;
}

static void prv_reset_hr_state(void) __attribute__((unused));
static void prv_reset_hr_state(void) {
    memset(s_hr_buf, 0, sizeof(s_hr_buf));
    s_hr_buf_idx     = 0;
    s_hr_buf_count   = 0;
    s_trigger_streak = 0;
    s_vmc_ema        = 0;
    s_hr_awake_baseline = 0;
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: HR state reset (fresh baseline)");
}

// ─── HR Accessibility Guard ───────────────────────────────────────────────────

/**
 * Returns true if the HR metric has valid data available for the past minute.
 * Use before every health_service_peek_current_value(HealthMetricHeartRateBPM).
 */
static bool prv_hr_accessible(void) {
    time_t now = time(NULL);
    HealthServiceAccessibilityMask mask =
        health_service_metric_accessible(HealthMetricHeartRateBPM, now - 60, now);
    return (mask & HealthServiceAccessibilityMaskAvailable) != 0;
}

// ─── HealthService Subscribe / Unsubscribe ───────────────────────────────────

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

// ─── Launch Logic ─────────────────────────────────────────────────────────────

static void prv_try_launch_foreground(void) {
    if (s_launch_pending)          return;
    if (prv_is_already_alarming()) return;
    if (prv_is_snoozed())          return;
    if (!prv_get_enabled())        return;
    if (!prv_is_in_window())       return;

    APP_LOG(APP_LOG_LEVEL_INFO,
        "NapBuster worker: sleep detected in no-nap window — launching app");

    s_launch_pending = true;
    // worker_launch_app() returns void — no return value to check
    worker_launch_app();

    // If app is already in foreground, also send a direct message
    AppWorkerMessage msg = { .data0 = WORKER_MSG_SLEEP_DETECTED };
    app_worker_send_message(WORKER_MSG_SLEEP_DETECTED, &msg);
}


// ─── Tier-1 Analysis (shared by HR event path and fallback timer) ─────────────

/**
 * Core Tier-1 analysis: called with a fresh HR reading (or 0 if unavailable).
 * Reads VMC from HealthMinuteData, updates rolling buffers, evaluates trigger
 * conditions.
 * hr_val: current BPM from either HealthEventHeartRateUpdate or a fresh peek.
 */
static void prv_run_tier1_analysis(int16_t hr_val) {
    // ── 1. Get VMC from HealthMinuteData (last 2 minutes) ────────────────────────
    // VMC (Vector Magnitude Count) is a pre-computed motion intensity from the
    // health subsystem — free, no extra sensor power, better calibrated than
    // raw accel_service_peek().
    uint32_t current_vmc = 0;
    HealthMinuteData minute_data[2];
    time_t end_time = time(NULL);
    time_t start_time = end_time - (2 * SECONDS_PER_MINUTE);
    uint32_t num_records = health_service_get_minute_history(
        minute_data, 2, &start_time, &end_time);
    if (num_records > 0) {
        // Use the most recent valid record's VMC
        for (int i = (int)num_records - 1; i >= 0; i--) {
            if (!minute_data[i].is_invalid) {
                current_vmc = minute_data[i].vmc;
                break;
            }
        }
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: VMC=%u (from %u records)", (unsigned)current_vmc, (unsigned)num_records);
    } else {
        // No minute history available — use EMA as safe fallback (not "still")
        current_vmc = s_vmc_ema > 0 ? s_vmc_ema : VMC_STILL_THRESH + 1;
        APP_LOG(APP_LOG_LEVEL_DEBUG, "NapBuster Tier1: no minute history, VMC fallback");
    }

    // ── 2. Update VMC EMA ─────────────────────────────────────────────────────────
    if (s_vmc_ema == 0) {
        s_vmc_ema = current_vmc;  // seed on first reading
    } else {
        s_vmc_ema = (s_vmc_ema * (VMC_EMA_ALPHA - 1) + current_vmc) / VMC_EMA_ALPHA;
    }

    // ── 3. Stillness check using VMC ──────────────────────────────────────────────
    bool still = (current_vmc < VMC_STILL_THRESH);

    // ── 4. Update HR smoothing buffer (last 3 readings) ──────────────────────
    // This is for smoothing current HR only — NOT used as the baseline.
    int16_t smoothed_hr = hr_val;
    if (hr_val > 0) {
        s_hr_buf[s_hr_buf_idx] = hr_val;
        s_hr_buf_idx = (s_hr_buf_idx + 1) % HR_BUF_SIZE;
        if (s_hr_buf_count < HR_BUF_SIZE) s_hr_buf_count++;

        // Compute smoothed HR from buffer
        int32_t sum = 0;
        for (uint8_t i = 0; i < s_hr_buf_count; i++) sum += s_hr_buf[i];
        smoothed_hr = (int16_t)(sum / s_hr_buf_count);
    }

    // ── 5. Update anchored awake baseline ────────────────────────────────────
    // Baseline is only established/updated when VMC is high (clearly moving/awake).
    // It never drifts downward — if HR drops (sleep onset), baseline stays put.
    // This prevents the "chasing" problem where rolling avg tracks sleep HR down.
    bool hr_drop = false;
    int32_t baseline_for_display = (int32_t)s_hr_awake_baseline;

    if (hr_val > 0) {
        if (s_hr_awake_baseline == 0) {
            // Not yet established — seed from first valid reading.
            // If this happens during exercise it'll correct itself quickly once
            // the resting zone is entered (VMC_BASELINE_MIN..VMC_BASELINE_MAX).
            s_hr_awake_baseline = smoothed_hr;
            APP_LOG(APP_LOG_LEVEL_INFO,
                "NapBuster Tier1: awake baseline seeded at %d BPM", (int)s_hr_awake_baseline);
        } else if (current_vmc >= VMC_BASELINE_MIN && current_vmc < VMC_BASELINE_MAX) {
            // VMC is in the "normal awake" zone — not asleep (< MIN) and not
            // exercising (>= MAX). Track HR freely with a gentle EMA so the
            // baseline converges to true resting awake HR over time.
            // Update in both directions so it isn't permanently stuck from an
            // early high/low seed — just moves slowly enough not to chase sleep onset.
            s_hr_awake_baseline = (int16_t)((s_hr_awake_baseline * 7 + smoothed_hr) / 8);
            APP_LOG(APP_LOG_LEVEL_DEBUG,
                "NapBuster Tier1: baseline tracking -> %d (vmc=%u in resting zone)",
                (int)s_hr_awake_baseline, (unsigned)current_vmc);
        } else {
            APP_LOG(APP_LOG_LEVEL_DEBUG,
                "NapBuster Tier1: baseline frozen (vmc=%u — %s)",
                (unsigned)current_vmc,
                current_vmc < VMC_BASELINE_MIN ? "too still/asleep" : "exercising");
        }

        // Check drop against ANCHORED baseline (not rolling average)
        if (s_hr_awake_baseline > 0 && s_hr_buf_count >= 2) {
            int drop_pct = prv_get_hr_drop_pct();
            hr_drop = ((int32_t)smoothed_hr * 100) < ((int32_t)s_hr_awake_baseline * drop_pct);
            APP_LOG(APP_LOG_LEVEL_DEBUG,
                "NapBuster Tier1: smoothed_hr=%d baseline=%d drop_pct=%d hr_drop=%d",
                (int)smoothed_hr, (int)s_hr_awake_baseline, drop_pct, (int)hr_drop);
        } else {
            APP_LOG(APP_LOG_LEVEL_DEBUG,
                "NapBuster Tier1: HR=%d baseline=%d buf_count=%d (need baseline+2 readings)",
                (int)hr_val, (int)s_hr_awake_baseline, (int)s_hr_buf_count);
        }
    }

    APP_LOG(APP_LOG_LEVEL_DEBUG,
        "NapBuster Tier1: vmc=%u ema=%u still=%d",
        (unsigned)current_vmc, (unsigned)s_vmc_ema, (int)still);

    // ── 6. Update trigger streak ──────────────────────────────────────────────
    if (hr_drop && still) {
        s_trigger_streak++;
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster Tier1: trigger streak=%d (need %d for full alarm)",
            (int)s_trigger_streak, STREAK_TO_FIRE);

        // x1: gentle nudge — launch foreground in nudge mode (double pulse, no alarm).
        // If this alone breaks the nap, great. If not, x2 fires the full alarm.
        if (s_trigger_streak == 1
                && prv_get_enabled()
                && prv_is_in_window()
                && !prv_is_snoozed()
                && !prv_is_already_alarming()) {
            persist_write_int(PERSIST_KEY_NUDGE_PENDING, 1);
            worker_launch_app();
            // Also message the foreground if it's already running
            AppWorkerMessage nudge_msg = { .data0 = WORKER_MSG_NAP_NUDGE };
            app_worker_send_message(WORKER_MSG_NAP_NUDGE, &nudge_msg);
            APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster Tier1: nudge launched at streak=1");
        }
    } else {
        if (s_trigger_streak > 0) {
            APP_LOG(APP_LOG_LEVEL_DEBUG,
                "NapBuster Tier1: streak reset (hr_drop=%d still=%d)",
                (int)hr_drop, (int)still);
        }
        s_trigger_streak = 0;
    }

    // ── 7. Fire full alarm if streak threshold reached ────────────────────────
    if (s_trigger_streak >= STREAK_TO_FIRE) {
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster Tier1: sleep detected — full alarm firing");
        s_trigger_streak = 0;  // reset so post-snooze doze-off can re-trigger
        prv_try_launch_foreground();
    }

    // ── 8. Persist all Tier-1 state ───────────────────────────────────────────
    // Write debug telemetry for the foreground display (PERSIST_KEY_DEBUG_ACCEL
    // now stores the current VMC reading instead of accel deviation)
    persist_write_int(PERSIST_KEY_DEBUG_HR,    (int)smoothed_hr);
    persist_write_int(PERSIST_KEY_DEBUG_AVG,   (int)baseline_for_display);
    persist_write_int(PERSIST_KEY_DEBUG_ACCEL, (int)current_vmc);
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
 * Fallback timer — fires every 5 minutes while inside the guard window.
 *
 * Skips analysis entirely if a HealthEventHeartRateUpdate arrived recently
 * (analysis already ran at that point — piggybacking was free, no duplication).
 * Only peeks HR ourselves when the OS hasn't sent an update, i.e. the user
 * has background HR sampling set to 30 min / 1 hour / off.
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
        // No recent HR event — do our own peek (guarded by accessibility check)
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1 timer: no recent HR event — peeking HR");
        int16_t hr_val = 0;
        if (prv_hr_accessible()) {
            HealthValue peeked =
                health_service_peek_current_value(HealthMetricHeartRateBPM);
            hr_val = (peeked > 0) ? (int16_t)peeked : 0;
        }
        prv_run_tier1_analysis(hr_val);
    }

    prv_start_sample_timer();  // re-arm
}

// ─── Window Boundary Timer ────────────────────────────────────────────────────

static void prv_window_timer_callback(void *ctx);  // fwd

static void prv_start_window_timer(void) {
    if (s_window_timer) return;
    s_window_timer = app_timer_register(WINDOW_CHECK_MS,
                                        prv_window_timer_callback, NULL);
}

static void prv_stop_window_timer(void) {
    if (s_window_timer) {
        app_timer_cancel(s_window_timer);
        s_window_timer = NULL;
    }
}

/**
 * Fires every 60 seconds.
 * Manages HealthService subscription and Tier-1 sample timer based on
 * whether we're currently inside the no-nap window.
 */
static void prv_window_timer_callback(void *ctx) {
    s_window_timer = NULL;  // timer fired, clear handle

    bool in_window = prv_get_enabled() && prv_is_in_window();

    if (in_window && !s_health_subscribed) {
        // ── Window just opened ────────────────────────────────────────────────
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker: window opened — HR buffer has %d readings (warm baseline)",
            (int)s_hr_buf_count);

        // Subscribe Tier-2 health events
        prv_subscribe_health();

        // Start Tier-1 sample timer on HR-capable platforms (may already be running)
        if (s_hr_capable) {
            // Do NOT reset HR state — carry the existing warm baseline in
            prv_start_sample_timer();
        }

        // Immediate sleep check in case we entered window while already asleep
        HealthActivityMask acts = health_service_peek_current_activities();
        if ((acts & HealthActivitySleep) || (acts & HealthActivityRestfulSleep)) {
            prv_try_launch_foreground();
        }

    } else if (!in_window && s_health_subscribed) {
        // ── Window just closed ────────────────────────────────────────────────
        APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: window closed");

        prv_unsubscribe_health();
        prv_stop_sample_timer();
        s_launch_pending = false;
    }

    // Re-arm for the next minute check
    prv_start_window_timer();
}

// ─── HealthService Callback (Tier 1 fast path + Tier 2 fallback) ─────────────

static void prv_health_event_handler(HealthEventType event, void *ctx) {
    // ── HealthEventSignificantUpdate — re-read everything ────────────────────
    if (event == HealthEventSignificantUpdate) {
        // Re-check sleep state — significant updates can include sleep transitions
        APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster: HealthEventSignificantUpdate — re-checking sleep");
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
        // ── Tier 1 fast path — piggyback on OS HR sample (zero extra battery) ──
        int16_t hr_val = 0;
        if (prv_hr_accessible()) {
            HealthValue peeked =
                health_service_peek_current_value(HealthMetricHeartRateBPM);
            hr_val = (peeked > 0) ? (int16_t)peeked : 0;
        }
        s_last_hr_event_time = time(NULL);
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: HR event — BPM=%d", (int)hr_val);
        prv_run_tier1_analysis(hr_val);
        return;
    }

    // ── Tier 2 — sleep confirmation event (all platforms) ────────────────────
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
            s_trigger_streak = 0;
            if (s_hr_capable) {
                persist_write_int(PERSIST_KEY_TRIGGER_STREAK, 0);
            }
            break;

        case APP_MSG_SETTINGS_CHANGED:
            s_launch_pending = false;
            s_trigger_streak = 0;
            // Re-evaluate window state immediately
            if (prv_get_enabled() && prv_is_in_window()) {
                prv_subscribe_health();
                if (s_hr_capable && !s_sample_timer) {
                    prv_start_sample_timer();
                }
            } else {
                prv_unsubscribe_health();
                prv_stop_sample_timer();
            }
            APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: settings reloaded");
            break;

        default:
            break;
    }
}

// ─── Worker Lifecycle ─────────────────────────────────────────────────────────

static void worker_init(void) {
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker v5: starting");

    app_worker_message_subscribe(prv_app_message_handler);

    // ── Runtime HR capability detection ──────────────────────────────────────
    // Use health_service_metric_accessible() for a proper capability check.
    // On basalt/chalk (no HR sensor) this returns 0 → graceful degradation to Tier 2.
    s_hr_capable = prv_hr_accessible();

    APP_LOG(APP_LOG_LEVEL_INFO,
        "NapBuster worker v5: HR capable=%d",
        (int)s_hr_capable);

    if (s_hr_capable) {
        prv_load_hr_state();
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker v5: loaded HR state idx=%d count=%d streak=%d vmc_ema=%u",
            (int)s_hr_buf_idx, (int)s_hr_buf_count,
            (int)s_trigger_streak, (unsigned)s_vmc_ema);
    }

    bool in_window = prv_get_enabled() && prv_is_in_window();

    if (s_hr_capable) {
        // ── Always subscribe health and start sample timer on HR-capable platforms ──
        // This keeps the HR buffer warm BEFORE the window opens, so we have a
        // full rolling baseline the moment guarding begins.
        // prv_try_launch_foreground() checks prv_is_in_window() so the alarm
        // CANNOT fire outside the window — collecting data outside is safe.
        prv_subscribe_health();
        prv_start_sample_timer();
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker v5: HR-capable — sampling always-on for warm baseline");
    }

    if (in_window) {
        // Subscribe Tier-2 health events (no-op if already subscribed above)
        prv_subscribe_health();

        if (s_hr_capable) {
            // Sample timer already started above — start is idempotent
            prv_start_sample_timer();
        }

        // Immediate check in case sleep is already active (Tier 2)
        HealthActivityMask acts = health_service_peek_current_activities();
        if ((acts & HealthActivitySleep) || (acts & HealthActivityRestfulSleep)) {
            APP_LOG(APP_LOG_LEVEL_INFO,
                "NapBuster worker v5: sleep already active on init");
            prv_try_launch_foreground();
        }
    } else {
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker v5: outside window on start — HealthService idle");
    }

    // Always start the 60s boundary check timer
    prv_start_window_timer();
}

static void worker_deinit(void) {
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker v5: stopping");

    prv_stop_sample_timer();
    prv_stop_window_timer();
    prv_unsubscribe_health();
    app_worker_message_unsubscribe();

    // Persist Tier-1 state for next restart
    if (s_hr_capable) {
        prv_save_hr_state();
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker v5: saved HR state idx=%d count=%d streak=%d",
            (int)s_hr_buf_idx, (int)s_hr_buf_count, (int)s_trigger_streak);
    }
}

// ─── Entry Point ─────────────────────────────────────────────────────────────

int main(void) {
    worker_init();
    worker_event_loop();
    worker_deinit();
    return 0;
}
