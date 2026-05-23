/**
 * common.h — NapBuster
 *
 * Shared constants, persist keys, vibe patterns, and setting helpers
 * used by both the foreground app (src/c/) and referenced by the worker.
 */

#pragma once
#include <pebble.h>

// ─── Persist Keys ────────────────────────────────────────────────────────────
#define PERSIST_KEY_ENABLED          0  // bool: master on/off
#define PERSIST_KEY_START_HOUR       1  // int:  window start hour (0-23)
#define PERSIST_KEY_END_HOUR         2  // int:  window end hour   (0-23)
#define PERSIST_KEY_SNOOZE_UNTIL     3  // time_t: snooze expiry epoch (0=none)
#define PERSIST_KEY_ALARMING         4  // bool: foreground alarm is active
#define PERSIST_KEY_WAKEUP_ID_SNOOZE 5  // WakeupId: scheduled snooze wakeup
#define PERSIST_KEY_VIBE_STRENGTH    7  // int:  0=Gentle 1=Medium 2=Strong
#define PERSIST_KEY_WEEKDAYS_ONLY    8  // bool: skip Sat+Sun

// ─── Defaults ────────────────────────────────────────────────────────────────
#define DEFAULT_ENABLED              1
#define DEFAULT_START_HOUR           11  // 11:00 AM
#define DEFAULT_END_HOUR             23  // 11:00 PM
#define DEFAULT_VIBE_STRENGTH        1   // Medium
#define DEFAULT_WEEKDAYS_ONLY        0   // guard every day

// ─── Worker / App Message Keys ────────────────────────────────────────────────
#define WORKER_MSG_SLEEP_DETECTED    0
#define APP_MSG_SNOOZE_10            10
#define APP_MSG_SNOOZE_30            11
#define APP_MSG_DISMISS              12
#define APP_MSG_SETTINGS_CHANGED     13

// ─── Wakeup Reason Codes ──────────────────────────────────────────────────────
#define WAKEUP_REASON_SNOOZE         42

// ─── Vibration Patterns ───────────────────────────────────────────────────────
// Pebble has no hardware intensity control; we simulate via pattern density.

// Gentle — two slow soft pulses, long rest
#define VIBE_GENTLE_LEN 4
static const uint32_t VIBE_GENTLE[VIBE_GENTLE_LEN] = {
    150, 300,   // soft pulse, pause
    150, 700    // soft pulse, long rest
};

// Medium — escalating buzz (original pattern)
#define VIBE_MEDIUM_LEN 8
static const uint32_t VIBE_MEDIUM[VIBE_MEDIUM_LEN] = {
    300, 100,   // short buzz
    300, 100,   // short buzz
    600, 200,   // long buzz
    600, 400    // long buzz, rest
};

// Strong — rapid-fire dense buzzing, minimal gaps
#define VIBE_STRONG_LEN 8
static const uint32_t VIBE_STRONG[VIBE_STRONG_LEN] = {
    500,  50,   // long buzz, tiny gap
    500,  50,   // long buzz, tiny gap
    500,  50,   // long buzz, tiny gap
    1000, 100   // sustained buzz, short rest
};

#define VIBE_STRENGTH_COUNT 3
static const char * const VIBE_STRENGTH_LABELS[VIBE_STRENGTH_COUNT] = {
    "Gentle", "Medium", "Strong"
};

// ─── Setting Helpers ──────────────────────────────────────────────────────────

static inline bool settings_get_enabled(void) {
    if (!persist_exists(PERSIST_KEY_ENABLED)) return DEFAULT_ENABLED;
    return (bool)persist_read_int(PERSIST_KEY_ENABLED);
}

static inline int settings_get_start_hour(void) {
    if (!persist_exists(PERSIST_KEY_START_HOUR)) return DEFAULT_START_HOUR;
    return persist_read_int(PERSIST_KEY_START_HOUR);
}

static inline int settings_get_end_hour(void) {
    if (!persist_exists(PERSIST_KEY_END_HOUR)) return DEFAULT_END_HOUR;
    return persist_read_int(PERSIST_KEY_END_HOUR);
}

static inline int settings_get_vibe_strength(void) {
    if (!persist_exists(PERSIST_KEY_VIBE_STRENGTH)) return DEFAULT_VIBE_STRENGTH;
    return persist_read_int(PERSIST_KEY_VIBE_STRENGTH);
}

static inline bool settings_get_weekdays_only(void) {
    if (!persist_exists(PERSIST_KEY_WEEKDAYS_ONLY)) return DEFAULT_WEEKDAYS_ONLY;
    return (bool)persist_read_int(PERSIST_KEY_WEEKDAYS_ONLY);
}

// ─── Window / Time Helpers ────────────────────────────────────────────────────

/** True if the current time falls within the configured no-nap window,
 *  respecting the weekdays-only setting. */
static inline bool is_in_no_nap_window(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    // Weekdays-only check (tm_wday: 0=Sun, 6=Sat)
    if (settings_get_weekdays_only()) {
        if (t->tm_wday == 0 || t->tm_wday == 6) return false;
    }

    int hour  = t->tm_hour;
    int start = settings_get_start_hour();
    int end   = settings_get_end_hour();

    if (start <= end) {
        return (hour >= start && hour < end);
    } else {
        // Crosses midnight (e.g. 22:00–06:00)
        return (hour >= start || hour < end);
    }
}
