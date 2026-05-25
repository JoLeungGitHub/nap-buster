/**
 * worker.c — NapBuster Background Worker v2
 *
 * Two-tier sleep detection:
 *
 *   TIER 1 (HR-capable platforms: emery/diorite)
 *   ─────────────────────────────────────────────
 *   Every 5 minutes inside the guard window:
 *     • Sample HR via health_service_peek_current_value(HealthMetricHeartRateBPM)
 *     • Sample accel via accel_service_peek() → integer magnitude
 *     • Maintain 8-sample rolling HR circular buffer (persisted)
 *     • Update accel EMA
 *     • If HR drops >13% below rolling avg AND accel is still for ≥2 consecutive
 *       5-min checks → fire alarm ~10-15 min into a nap (much earlier than native)
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
 *   - accel_service_peek() returns 0 on success, non-zero on failure
 *   - health_service_peek_current_value returns HealthValue (int32_t)
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
#define PERSIST_KEY_ACCEL_AVG         14  // int32_t EMA of accel magnitude

// ─── Defaults ─────────────────────────────────────────────────────────────────

#define DEFAULT_ENABLED                1
#define DEFAULT_START_HOUR             11
#define DEFAULT_END_HOUR               23
#define DEFAULT_ACTIVE_DAYS            0x7F  // every day

// ─── Messages ─────────────────────────────────────────────────────────────────

#define WORKER_MSG_SLEEP_DETECTED      0
#define APP_MSG_SNOOZE_10              10
#define APP_MSG_SNOOZE_30              11
#define APP_MSG_DISMISS                12
#define APP_MSG_SETTINGS_CHANGED       13

// ─── Tier-1 Constants ─────────────────────────────────────────────────────────

#define HR_BUF_SIZE          8       // ~40 min history at 5-min intervals
#define HR_DROP_PCT          87      // trigger if hr*100 < rolling_avg * HR_DROP_PCT
#define ACCEL_STILL_THRESH   200     // milli-g deviation from EMA = "still"
#define ACCEL_EMA_ALPHA      8       // EMA weight: new = (old*(ALPHA-1) + new) / ALPHA
#define STREAK_TO_FIRE       2       // consecutive detections before alarm
#define SAMPLE_INTERVAL_MS   300000  // 5 minutes in ms
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

// Tier-1 accel EMA (milli-g)
static int32_t  s_accel_avg    = 0;

// ─── Integer sqrt (Newton's method — no math.h needed) ───────────────────────

static int32_t prv_isqrt(int64_t sq) {
    if (sq <= 0) return 0;
    int64_t r = sq / 2 + 1;
    for (int i = 0; i < 10; i++) {
        int64_t r2 = (r + sq / r) / 2;
        if (r2 >= r) break;
        r = r2;
    }
    return (int32_t)r;
}

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
    persist_write_int(PERSIST_KEY_ACCEL_AVG,       s_accel_avg);
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
    s_accel_avg       = persist_exists(PERSIST_KEY_ACCEL_AVG)
                        ? (int32_t)persist_read_int(PERSIST_KEY_ACCEL_AVG)      : 0;
}

static void prv_reset_hr_state(void) {
    memset(s_hr_buf, 0, sizeof(s_hr_buf));
    s_hr_buf_idx     = 0;
    s_hr_buf_count   = 0;
    s_trigger_streak = 0;
    s_accel_avg      = 0;
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: HR state reset (fresh baseline)");
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

// ─── Tier-1 Sample Timer ──────────────────────────────────────────────────────

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
 * Fires every 5 minutes while inside the guard window AND s_hr_capable.
 *
 * 1. Sample HR + accel
 * 2. Update rolling buffers
 * 3. Check two-condition trigger (hr_drop AND still)
 * 4. Fire alarm if streak >= STREAK_TO_FIRE
 * 5. Persist all state
 */
static void prv_sample_timer_callback(void *ctx) {
    s_sample_timer = NULL;  // timer fired, clear handle

    // ── 1. Sample heart rate ──────────────────────────────────────────────────
    HealthValue hr_val =
        health_service_peek_current_value(HealthMetricHeartRateBPM);
    int16_t current_hr = (hr_val > 0) ? (int16_t)hr_val : 0;

    // ── 2. Sample accelerometer ───────────────────────────────────────────────
    AccelData accel = {0};
    int32_t current_mag = 0;
    if (accel_service_peek(&accel) == 0) {
        // Compute magnitude sqrt(x²+y²+z²) in milli-g
        int64_t sq = (int64_t)accel.x * accel.x
                   + (int64_t)accel.y * accel.y
                   + (int64_t)accel.z * accel.z;
        current_mag = prv_isqrt(sq);
    } else {
        APP_LOG(APP_LOG_LEVEL_WARNING,
            "NapBuster worker: accel_service_peek failed");
        // Use EMA as fallback so we don't get a false "still" reading
        current_mag = s_accel_avg;
    }

    // ── 3. Update accel EMA ───────────────────────────────────────────────────
    if (s_accel_avg == 0) {
        // First reading — seed EMA
        s_accel_avg = current_mag;
    } else {
        s_accel_avg = (s_accel_avg * (ACCEL_EMA_ALPHA - 1) + current_mag)
                      / ACCEL_EMA_ALPHA;
    }

    // ── 4. Update HR circular buffer ─────────────────────────────────────────
    if (current_hr > 0) {
        s_hr_buf[s_hr_buf_idx] = current_hr;
        s_hr_buf_idx = (s_hr_buf_idx + 1) % HR_BUF_SIZE;
        if (s_hr_buf_count < HR_BUF_SIZE) s_hr_buf_count++;
    }

    // ── 5. Compute rolling HR average ─────────────────────────────────────────
    bool hr_drop = false;
    if (current_hr > 0 && s_hr_buf_count >= 3) {
        int32_t sum = 0;
        for (uint8_t i = 0; i < s_hr_buf_count; i++) {
            sum += s_hr_buf[i];
        }
        int32_t rolling_avg = sum / s_hr_buf_count;

        // Trigger if current HR is more than 13% below recent rolling average
        hr_drop = ((int32_t)current_hr * 100) < (rolling_avg * HR_DROP_PCT);

        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: HR=%d avg=%d hr_drop=%d",
            (int)current_hr, (int)rolling_avg, (int)hr_drop);
    } else {
        APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster Tier1: HR=%d buf_count=%d (need >=3)",
            (int)current_hr, (int)s_hr_buf_count);
    }

    // ── 6. Stillness check ────────────────────────────────────────────────────
    int32_t accel_dev = current_mag - s_accel_avg;
    if (accel_dev < 0) accel_dev = -accel_dev;
    bool still = (accel_dev < ACCEL_STILL_THRESH);

    APP_LOG(APP_LOG_LEVEL_DEBUG,
        "NapBuster Tier1: mag=%d ema=%d dev=%d still=%d",
        (int)current_mag, (int)s_accel_avg, (int)accel_dev, (int)still);

    // ── 7. Update trigger streak ──────────────────────────────────────────────
    if (hr_drop && still) {
        s_trigger_streak++;
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster Tier1: trigger streak=%d (need %d)",
            (int)s_trigger_streak, STREAK_TO_FIRE);
    } else {
        if (s_trigger_streak > 0) {
            APP_LOG(APP_LOG_LEVEL_DEBUG,
                "NapBuster Tier1: streak reset (hr_drop=%d still=%d)",
                (int)hr_drop, (int)still);
        }
        s_trigger_streak = 0;
    }

    // ── 8. Fire if streak threshold reached ───────────────────────────────────
    if (s_trigger_streak >= STREAK_TO_FIRE) {
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster Tier1: sleep detected — HR+accel threshold reached");
        s_trigger_streak = 0;  // reset so a post-snooze doze-off can re-trigger
        prv_try_launch_foreground();
    }

    // ── 9. Persist all Tier-1 state ───────────────────────────────────────────
    prv_save_hr_state();

    // ── 10. Re-arm for next 5-minute sample ───────────────────────────────────
    prv_start_sample_timer();
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
        APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: window opened");

        // Subscribe Tier-2 health events
        prv_subscribe_health();

        // Start Tier-1 sample timer on HR-capable platforms
        if (s_hr_capable) {
            // Fresh baseline — don't carry stale HR from outside the window
            prv_reset_hr_state();
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

// ─── Tier-2 HealthService Callback ───────────────────────────────────────────

static void prv_health_event_handler(HealthEventType event, void *ctx) {
    // Event-driven — fires when Pebble's health engine emits a state change.
    HealthActivityMask activities = health_service_peek_current_activities();

    bool is_sleeping = (activities & HealthActivitySleep) ||
                       (activities & HealthActivityRestfulSleep);

    if (is_sleeping) {
        if (s_hr_capable) {
            APP_LOG(APP_LOG_LEVEL_INFO,
                "NapBuster Tier2 (fallback): sleep event on HR-capable platform");
        } else {
            APP_LOG(APP_LOG_LEVEL_INFO,
                "NapBuster Tier2: sleep event detected");
        }
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
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker v2: starting");

    app_worker_message_subscribe(prv_app_message_handler);

    // ── Runtime HR capability detection ──────────────────────────────────────
    // On HR-capable platforms (emery/diorite) this returns a positive BPM.
    // On basalt/chalk it returns 0 or negative — graceful degradation to Tier 2.
    HealthValue hr_probe =
        health_service_peek_current_value(HealthMetricHeartRateBPM);
    s_hr_capable = (hr_probe > 0);

    APP_LOG(APP_LOG_LEVEL_INFO,
        "NapBuster worker v2: HR capable=%d (probe=%d)",
        (int)s_hr_capable, (int)hr_probe);

    if (s_hr_capable) {
        prv_load_hr_state();
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker v2: loaded HR state idx=%d count=%d streak=%d accel_avg=%d",
            (int)s_hr_buf_idx, (int)s_hr_buf_count,
            (int)s_trigger_streak, (int)s_accel_avg);
    }

    bool in_window = prv_get_enabled() && prv_is_in_window();

    if (in_window) {
        // Subscribe Tier-2 health events immediately
        prv_subscribe_health();

        if (s_hr_capable) {
            // Start Tier-1 sample timer
            prv_start_sample_timer();
        }

        // Immediate check in case sleep is already active (Tier 2)
        HealthActivityMask acts = health_service_peek_current_activities();
        if ((acts & HealthActivitySleep) || (acts & HealthActivityRestfulSleep)) {
            APP_LOG(APP_LOG_LEVEL_INFO,
                "NapBuster worker v2: sleep already active on init");
            prv_try_launch_foreground();
        }
    } else {
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker v2: outside window on start — HealthService idle");
    }

    // Always start the 60s boundary check timer
    prv_start_window_timer();
}

static void worker_deinit(void) {
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker v2: stopping");

    prv_stop_sample_timer();
    prv_stop_window_timer();
    prv_unsubscribe_health();
    app_worker_message_unsubscribe();

    // Persist Tier-1 state for next restart
    if (s_hr_capable) {
        prv_save_hr_state();
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker v2: saved HR state idx=%d count=%d streak=%d",
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
