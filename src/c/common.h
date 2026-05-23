/**
 * common.h — NapBuster
 *
 * Shared constants, persist keys, data structures, and messaging
 * definitions used by both the foreground app and the background worker.
 */

#pragma once
#include <pebble.h>

// ─── Persist Keys ────────────────────────────────────────────────────────────
#define PERSIST_KEY_ENABLED         0   // bool: 0 = disabled, 1 = enabled
#define PERSIST_KEY_START_HOUR      1   // int:  no-nap window start (0–23)
#define PERSIST_KEY_END_HOUR        2   // int:  no-nap window end   (0–23)
#define PERSIST_KEY_SNOOZE_UNTIL    3   // time_t: epoch when snooze expires (0 = none)
#define PERSIST_KEY_ALARMING        4   // bool: 1 = alarm is currently active
#define PERSIST_KEY_WAKEUP_ID       5   // WakeupId: stored wakeup for snooze

// ─── Default Settings ────────────────────────────────────────────────────────
#define DEFAULT_ENABLED             1
#define DEFAULT_START_HOUR          11  // 11:00 AM
#define DEFAULT_END_HOUR            23  // 11:00 PM

// ─── App Message / Worker Message Keys ───────────────────────────────────────
#define WORKER_MSG_SLEEP_DETECTED   0   // worker → app: sleep detected in window
#define WORKER_MSG_SNOOZE_EXPIRED   1   // worker → app: snooze time has passed
#define APP_MSG_SNOOZE_10           10  // app → worker: snooze 10 min
#define APP_MSG_SNOOZE_30           11  // app → worker: snooze 30 min
#define APP_MSG_DISMISS             12  // app → worker: alarm dismissed
#define APP_MSG_SETTINGS_CHANGED    13  // app → worker: re-read settings

// ─── Wakeup IDs (two separate wakeup slots) ──────────────────────────────────
#define PERSIST_KEY_WAKEUP_ID_SNOOZE   5   // WakeupId for snooze re-arm
#define PERSIST_KEY_WAKEUP_ID_WINDOW   6   // WakeupId for window start re-arm

// ─── Wakeup Reason Codes ─────────────────────────────────────────────────────
#define WAKEUP_REASON_SNOOZE        42  // snooze expired → restart alarm
#define WAKEUP_REASON_WINDOW_START  43  // window opens → restart worker

// ─── Vibration Pattern ───────────────────────────────────────────────────────
#define VIBE_PATTERN_SEGMENTS_LEN   8
static const uint32_t VIBE_SEGMENTS[VIBE_PATTERN_SEGMENTS_LEN] = {
    300, 100,   // short buzz, short pause
    300, 100,   // short buzz, short pause
    600, 200,   // long buzz,  longer pause
    600, 400    // long buzz,  rest
};

// ─── Helper: read settings with safe defaults ────────────────────────────────
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

// ─── Helper: check if current time falls in the no-nap window ────────────────
static inline bool is_in_no_nap_window(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int hour  = t->tm_hour;
    int start = settings_get_start_hour();
    int end   = settings_get_end_hour();

    if (start <= end) {
        return (hour >= start && hour < end);
    } else {
        // Crosses midnight (e.g. 22:00–08:00)
        return (hour >= start || hour < end);
    }
}
