/**
 * worker.c — NapBuster Background Worker
 *
 * Battery-efficient design:
 *   The worker stays running but subscribes/unsubscribes HealthService
 *   based on whether we're inside the no-nap window.
 *
 *   OUTSIDE window → HealthService UNSUBSCRIBED (zero sensor overhead)
 *                    A 60-second timer polls the clock to detect window open
 *
 *   INSIDE window  → HealthService SUBSCRIBED (event-driven, no busy poll)
 *                    The 60-second timer also detects window close
 *
 * Since workers can't schedule Wakeups or call worker_event_loop_quit(),
 * this is the correct minimal-overhead approach available in the worker API.
 *
 * Key SDK notes:
 *   - worker_launch_app() returns void (no return value)
 *   - WakeupId / wakeup_schedule() NOT available in worker context
 *   - HealthActivitySleep (NOT HealthActivityMaskSleep) is the correct enum
 */

#include <pebble_worker.h>

// ─── Shared constants (inlined — worker can't use pebble.h) ──────────────────

#define PERSIST_KEY_ENABLED            0
#define PERSIST_KEY_START_HOUR         1
#define PERSIST_KEY_END_HOUR           2
#define PERSIST_KEY_SNOOZE_UNTIL       3
#define PERSIST_KEY_ALARMING           4

#define DEFAULT_ENABLED                1
#define DEFAULT_START_HOUR             11
#define DEFAULT_END_HOUR               23

#define WORKER_MSG_SLEEP_DETECTED      0
#define APP_MSG_SNOOZE_10              10
#define APP_MSG_SNOOZE_30              11
#define APP_MSG_DISMISS                12
#define APP_MSG_SETTINGS_CHANGED       13

// ─── State ───────────────────────────────────────────────────────────────────

// True once we've called worker_launch_app() and haven't gotten a dismiss yet
static bool s_launch_pending    = false;

// True when HealthService is currently subscribed
static bool s_health_subscribed = false;

// 60-second timer: used to check window open/close boundary
static AppTimer *s_window_timer = NULL;

// ─── Setting Helpers ─────────────────────────────────────────────────────────

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
    int hour  = t->tm_hour;
    int start = prv_get_start_hour();
    int end   = prv_get_end_hour();

    if (start <= end) {
        return (hour >= start && hour < end);
    } else {
        // Crosses midnight (e.g. 22:00–08:00)
        return (hour >= start || hour < end);
    }
}

// ─── Guard Helpers ───────────────────────────────────────────────────────────

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

// ─── HealthService Subscribe / Unsubscribe ───────────────────────────────────

// Forward declaration
static void prv_health_event_handler(HealthEventType event, void *ctx);

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

    // If app is already in foreground (already open), also send a direct message
    AppWorkerMessage msg = { .data0 = WORKER_MSG_SLEEP_DETECTED };
    app_worker_send_message(WORKER_MSG_SLEEP_DETECTED, &msg);
}

// ─── Window Boundary Timer ────────────────────────────────────────────────────

// Forward declaration
static void prv_window_timer_callback(void *ctx);

static void prv_start_window_timer(void) {
    if (s_window_timer) return;
    s_window_timer = app_timer_register(60 * 1000, prv_window_timer_callback, NULL);
}

static void prv_stop_window_timer(void) {
    if (s_window_timer) {
        app_timer_cancel(s_window_timer);
        s_window_timer = NULL;
    }
}

/**
 * Fires every 60 seconds.
 * Subscribes or unsubscribes HealthService based on whether we're
 * currently inside the no-nap window. This is the only "polling" — one
 * clock comparison per minute, with no sensor activity outside the window.
 */
static void prv_window_timer_callback(void *ctx) {
    s_window_timer = NULL;  // timer fired, clear handle

    bool in_window = prv_get_enabled() && prv_is_in_window();

    if (in_window && !s_health_subscribed) {
        APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: window opened — subscribing");
        prv_subscribe_health();

        // Immediate check in case we were already asleep when window opened
        HealthActivityMask acts = health_service_peek_current_activities();
        if ((acts & HealthActivitySleep) || (acts & HealthActivityRestfulSleep)) {
            prv_try_launch_foreground();
        }
    } else if (!in_window && s_health_subscribed) {
        APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: window closed — unsubscribing");
        prv_unsubscribe_health();
        s_launch_pending = false;
    }

    // Re-arm for the next minute check
    prv_start_window_timer();
}

// ─── HealthService Callback ───────────────────────────────────────────────────

static void prv_health_event_handler(HealthEventType event, void *ctx) {
    // Event-driven — fires only when Pebble's health engine emits a state change.
    HealthActivityMask activities = health_service_peek_current_activities();

    bool is_sleeping = (activities & HealthActivitySleep) ||
                       (activities & HealthActivityRestfulSleep);

    if (is_sleeping) {
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
            break;

        case APP_MSG_SETTINGS_CHANGED:
            s_launch_pending = false;
            // Re-evaluate window state immediately
            if (prv_get_enabled() && prv_is_in_window()) {
                prv_subscribe_health();
            } else {
                prv_unsubscribe_health();
            }
            APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: settings reloaded");
            break;

        default:
            break;
    }
}

// ─── Worker Lifecycle ─────────────────────────────────────────────────────────

static void worker_init(void) {
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: starting");

    app_worker_message_subscribe(prv_app_message_handler);

    bool in_window = prv_get_enabled() && prv_is_in_window();

    if (in_window) {
        // Subscribe immediately — we're inside the guard window
        prv_subscribe_health();

        // Immediate check in case sleep is already active
        HealthActivityMask acts = health_service_peek_current_activities();
        if ((acts & HealthActivitySleep) || (acts & HealthActivityRestfulSleep)) {
            APP_LOG(APP_LOG_LEVEL_INFO,
                "NapBuster worker: sleep active on init");
            prv_try_launch_foreground();
        }
    } else {
        APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster worker: outside window on start — HealthService idle");
        // HealthService stays unsubscribed — zero overhead until window opens
    }

    // Always start the 60s boundary check timer
    prv_start_window_timer();
}

static void worker_deinit(void) {
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster worker: stopping");
    prv_stop_window_timer();
    prv_unsubscribe_health();
    app_worker_message_unsubscribe();
}

// ─── Entry Point ─────────────────────────────────────────────────────────────

int main(void) {
    worker_init();
    worker_event_loop();
    worker_deinit();
    return 0;
}
