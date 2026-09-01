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
#define PERSIST_KEY_ACTIVE_DAYS      8  // uint8 bitmask: bit0=Sun..bit6=Sat

// ─── Tier-1 debug / tuning persist keys ──────────────────────────────────────
#define PERSIST_KEY_TRIGGER_STREAK   13  // uint8: consecutive positive evidence, whole minutes
#define PERSIST_KEY_DEBUG_HR         15  // int16: last smoothed HR BPM
#define PERSIST_KEY_DEBUG_AVG        16  // int16: anchored awake HR baseline
#define PERSIST_KEY_DEBUG_ACCEL      17  // int32: last VMC reading (Vector Magnitude Count)
#define PERSIST_KEY_SENSITIVITY      18  // int: 0=Sensitive 1=Balanced 2=Conservative
#define PERSIST_KEY_HR_BASELINE      19  // int16: anchored awake HR baseline
#define PERSIST_KEY_NUDGE_PENDING    20  // bool: worker requests a nudge pulse from foreground
#define PERSIST_KEY_LAST_DISMISS     21  // time_t: last alarm dismissal (worker cooldown gate)
#define PERSIST_KEY_STREAK_START     22  // legacy detector transient (cleared by schema migration)
#define PERSIST_KEY_DEBUG_LAST_TS    23  // time_t: when the worker last completed an analysis
#define PERSIST_KEY_HRV_BASELINE     24  // legacy HRV baseline (unused by detector v2.5)
#define PERSIST_KEY_DEBUG_HRV        25  // int: candidate PPI RMSSD diagnostic (ms), -1=unavailable
#define PERSIST_KEY_ALARM_START      26  // time_t: when the foreground alarm began
#define PERSIST_KEY_DETECTOR_SCHEMA  27  // int: persisted detector-state schema version
#define PERSIST_KEY_LAST_NUDGE       28  // time_t: last nudge (cooldown and diagnostics)
#define PERSIST_KEY_DEBUG_PHASE      29  // int: 0=Armed 1=Candidate 2=Nudged
#define PERSIST_KEY_WORKER_STATUS    30  // uint8 bitmask: worker/sensor readiness

// ─── Defaults ────────────────────────────────────────────────────────────────
#define DEFAULT_ENABLED              1
#define DEFAULT_START_HOUR           11  // 11:00 AM
#define DEFAULT_END_HOUR             23  // 11:00 PM
#define DEFAULT_VIBE_STRENGTH        1   // Medium
#define DEFAULT_ACTIVE_DAYS          0x7F  // every day (bits 0-6 set)
#define DEFAULT_SENSITIVITY          1   // Balanced (12% full HR drop)

// ─── Worker / App Message Keys ────────────────────────────────────────────────
#define WORKER_MSG_SLEEP_DETECTED    0
#define WORKER_MSG_NAP_NUDGE         1  // candidate sustained: gentle nudge (no full alarm)
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

static inline uint8_t settings_get_active_days(void) {
    if (!persist_exists(PERSIST_KEY_ACTIVE_DAYS)) return DEFAULT_ACTIVE_DAYS;
    // A stored 0 would mean "never guard" — treat as corrupt (e.g. leftover
    // bool from the pre-1.x weekdays toggle that shared this key) and default.
    uint8_t days = (uint8_t)persist_read_int(PERSIST_KEY_ACTIVE_DAYS) & 0x7F;
    return days ? days : DEFAULT_ACTIVE_DAYS;
}

static inline int settings_get_sensitivity(void) {
    if (!persist_exists(PERSIST_KEY_SENSITIVITY)) return DEFAULT_SENSITIVITY;
    return persist_read_int(PERSIST_KEY_SENSITIVITY);
}

// Detector phase values written by the worker for foreground diagnostics.
#define DETECTOR_PHASE_ARMED      0
#define DETECTOR_PHASE_CANDIDATE  1
#define DETECTOR_PHASE_NUDGED     2

// Worker-status bits written to PERSIST_KEY_WORKER_STATUS.
#define WORKER_STATUS_RUNNING        (1 << 0)
#define WORKER_STATUS_HEALTH_ACTIVE  (1 << 1)
#define WORKER_STATUS_HR_CAPABLE     (1 << 2)
#define WORKER_STATUS_HR_ACTIVE      (1 << 3)
#define WORKER_STATUS_HRV_ACTIVE     (1 << 4)
#define WORKER_STATUS_MOTION_FRESH   (1 << 5)
#define WORKER_STATUS_BASELINE_READY (1 << 6)

// ─── Window / Time Helpers ────────────────────────────────────────────────────

/** True if the current time falls within the configured no-nap window.
 *
 * For a window that crosses midnight, the after-midnight portion belongs to
 * the day on which the window started.  For example, a Monday-only 22:00-06:00
 * window remains active until 06:00 Tuesday.  Equal start/end hours mean an
 * all-day window; the master toggle remains the way to disable guarding. */
static inline bool is_in_no_nap_window(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    int hour  = t->tm_hour;
    int start = settings_get_start_hour();
    int end   = settings_get_end_hour();

    int window_day = t->tm_wday;
    if (start > end && hour < end) {
        window_day = (window_day + 6) % 7;  // after midnight: previous day
    }

    uint8_t active_days = settings_get_active_days();
    if (!((active_days >> window_day) & 1)) return false;

    if (start == end) {
        return true;
    } else if (start < end) {
        return (hour >= start && hour < end);
    } else {
        // Crosses midnight (e.g. 22:00–06:00)
        return (hour >= start || hour < end);
    }
}
