/**
 * worker.c - NapBuster background-worker adapter
 *
 * Detection itself lives in nap_detector.c and has no Pebble dependencies.
 * This file owns the platform edges: fresh HealthService events, completed
 * minute motion summaries, sensor cadence, schedule boundaries, persistence,
 * foreground launches, and diagnostic telemetry.
 */

#include <pebble_worker.h>

#include "nap_detector.h"

#include <string.h>

// Shared settings/state keys (kept in sync with src/c/common.h).
#define PERSIST_KEY_ENABLED             0
#define PERSIST_KEY_START_HOUR          1
#define PERSIST_KEY_END_HOUR            2
#define PERSIST_KEY_SNOOZE_UNTIL        3
#define PERSIST_KEY_ALARMING            4
#define PERSIST_KEY_ACTIVE_DAYS         8

// Legacy detector keys. They are removed by the one-time schema migration.
#define PERSIST_KEY_HR_BUFFER           10
#define PERSIST_KEY_HR_BUF_IDX          11
#define PERSIST_KEY_HR_BUF_COUNT        12
#define PERSIST_KEY_TRIGGER_STREAK      13
#define PERSIST_KEY_VMC_EMA             14

// Foreground diagnostics/settings.
#define PERSIST_KEY_DEBUG_HR            15
#define PERSIST_KEY_DEBUG_AVG           16
#define PERSIST_KEY_DEBUG_ACCEL         17
#define PERSIST_KEY_SENSITIVITY         18
#define PERSIST_KEY_HR_BASELINE         19
#define PERSIST_KEY_NUDGE_PENDING       20
#define PERSIST_KEY_LAST_DISMISS        21
#define PERSIST_KEY_STREAK_START        22
#define PERSIST_KEY_DEBUG_LAST_TS       23
#define PERSIST_KEY_HRV_BASELINE        24
#define PERSIST_KEY_DEBUG_HRV           25
#define PERSIST_KEY_ALARM_START         26

// Detector-v3 persistence/telemetry.
#define PERSIST_KEY_DETECTOR_SCHEMA     27
#define PERSIST_KEY_LAST_NUDGE          28
#define PERSIST_KEY_DEBUG_PHASE         29
#define PERSIST_KEY_WORKER_STATUS       30
#define DETECTOR_SCHEMA_VERSION          1

#define DEFAULT_ENABLED                  1
#define DEFAULT_START_HOUR              11
#define DEFAULT_END_HOUR                23
#define DEFAULT_ACTIVE_DAYS             0x7F
#define DEFAULT_SENSITIVITY              1

#define WORKER_MSG_SLEEP_DETECTED        0
#define WORKER_MSG_NAP_NUDGE             1
#define APP_MSG_SNOOZE_10               10
#define APP_MSG_SNOOZE_30               11
#define APP_MSG_DISMISS                 12
#define APP_MSG_SETTINGS_CHANGED       13

#define WARM_LEAD_HOURS                  2
#define HR_ARMED_PERIOD_SECS           120
#define HR_CONFIRM_PERIOD_SECS          20
#define HRV_CONFIRM_PERIOD_SECS         20
#define SENSOR_RETRY_SECS              300

#define MOTION_WINDOW_MINUTES            5
#define MOTION_QUIET_VMC_MAX           100
#define NUDGE_COOLDOWN_SECS             600
#define DISMISS_COOLDOWN_SECS           600
#define SNOOZE_MAX_SECS                7200
#define ALARM_STALE_SECS               1800
#define LAUNCH_PENDING_SECS             300

#define PPI_RING_SIZE                    32
#define PPI_MIN_SAMPLES                   4
#define PPI_MIN_MS                      300
#define PPI_MAX_MS                     2000
#define PPI_HR_TOLERANCE_PCT             40
#define PPI_CONTIGUOUS_GAP_SECS           3
#define PPI_FRESH_SECS                   60

// PERSIST_KEY_WORKER_STATUS bitfield.
#define WORKER_STATUS_RUNNING          (1 << 0)
#define WORKER_STATUS_HEALTH           (1 << 1)
#define WORKER_STATUS_HR_CAPABLE       (1 << 2)
#define WORKER_STATUS_HR_ACTIVE        (1 << 3)
#define WORKER_STATUS_HRV_ACTIVE       (1 << 4)
#define WORKER_STATUS_MOTION_FRESH     (1 << 5)
#define WORKER_STATUS_BASELINE_READY   (1 << 6)

static NapDetector s_detector;
static uint16_t s_saved_baseline;
static int s_last_written_status = -1;
static int s_last_written_phase = -1;
static int s_last_written_rmssd = -32768;
static time_t s_last_telemetry_time;

static bool s_health_subscribed;
static bool s_hr_capable;
static bool s_window_active;
static bool s_last_motion_fresh;

static uint16_t s_hr_period;
static uint16_t s_hrv_period;
static time_t s_hr_retry_after;
static time_t s_hrv_retry_after;

static bool s_launch_pending;
static time_t s_launch_pending_time;
static time_t s_last_nudge_time;
static bool s_observed_alarming;

static int16_t s_last_hr_bpm;
static time_t s_last_hr_time;
static time_t s_last_accepted_hr_time;
static uint16_t s_ppi_ring[PPI_RING_SIZE];
static uint8_t s_ppi_count;
static uint8_t s_ppi_index;
static time_t s_last_ppi_time;

static void prv_health_event_handler(HealthEventType event, void *context);
static void prv_apply_window_state(void);

// Settings and schedule ------------------------------------------------------

static bool prv_get_enabled(void) {
    return !persist_exists(PERSIST_KEY_ENABLED) ||
           (bool)persist_read_int(PERSIST_KEY_ENABLED);
}

static int prv_get_hour(int key, int fallback) {
    int hour = persist_exists(key) ? persist_read_int(key) : fallback;
    return (hour >= 0 && hour <= 23) ? hour : fallback;
}

static int prv_get_start_hour(void) {
    return prv_get_hour(PERSIST_KEY_START_HOUR, DEFAULT_START_HOUR);
}

static int prv_get_end_hour(void) {
    return prv_get_hour(PERSIST_KEY_END_HOUR, DEFAULT_END_HOUR);
}

static uint8_t prv_get_active_days(void) {
    uint8_t days = persist_exists(PERSIST_KEY_ACTIVE_DAYS)
                       ? (uint8_t)persist_read_int(PERSIST_KEY_ACTIVE_DAYS)
                       : DEFAULT_ACTIVE_DAYS;
    days &= 0x7F;
    return days == 0 ? DEFAULT_ACTIVE_DAYS : days;
}

static NapDetectorSensitivity prv_get_sensitivity(void) {
    int value = persist_exists(PERSIST_KEY_SENSITIVITY)
                    ? persist_read_int(PERSIST_KEY_SENSITIVITY)
                    : DEFAULT_SENSITIVITY;
    if (value == NAP_DETECTOR_SENSITIVE ||
        value == NAP_DETECTOR_CONSERVATIVE) {
        return (NapDetectorSensitivity)value;
    }
    return NAP_DETECTOR_BALANCED;
}

static bool prv_day_is_active(uint8_t days, int weekday) {
    return ((days >> weekday) & 1u) != 0;
}

/**
 * A cross-midnight window belongs to the weekday on which it opened. Equal
 * start/end hours mean all day; the master toggle is how guarding is disabled.
 */
static bool prv_is_in_window(void) {
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    int hour = local->tm_hour;
    int weekday = local->tm_wday;
    int start = prv_get_start_hour();
    int end = prv_get_end_hour();
    uint8_t days = prv_get_active_days();

    if (start == end) {
        return prv_day_is_active(days, weekday);
    }
    if (start < end) {
        return hour >= start && hour < end &&
               prv_day_is_active(days, weekday);
    }
    if (hour >= start) {
        return prv_day_is_active(days, weekday);
    }
    if (hour < end) {
        return prv_day_is_active(days, (weekday + 6) % 7);
    }
    return false;
}

/** True during the two hours preceding the next active guard-day start. */
static bool prv_is_in_lead_period(void) {
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    int hour = local->tm_hour;
    int weekday = local->tm_wday;
    int start = prv_get_start_hour();
    int end = prv_get_end_hour();
    int lead_start = (start + 24 - WARM_LEAD_HOURS) % 24;
    int guard_weekday;
    bool in_lead;

    if (start == end) {
        // An all-day guard starts at midnight, regardless of the stored hour.
        in_lead = hour >= (24 - WARM_LEAD_HOURS);
        guard_weekday = (weekday + 1) % 7;
    } else if (lead_start < start) {
        in_lead = hour >= lead_start && hour < start;
        guard_weekday = weekday;
    } else {
        // For a midnight/01:00 start, the pre-midnight portion is preparing
        // tomorrow's guard day; the after-midnight portion prepares today.
        in_lead = hour >= lead_start || hour < start;
        guard_weekday = hour >= lead_start ? (weekday + 1) % 7 : weekday;
    }

    return in_lead && prv_day_is_active(prv_get_active_days(), guard_weekday);
}

// Persistence and diagnostics -----------------------------------------------

static void prv_migrate_detector_state(void) {
    if (persist_exists(PERSIST_KEY_DETECTOR_SCHEMA) &&
        persist_read_int(PERSIST_KEY_DETECTOR_SCHEMA) ==
            DETECTOR_SCHEMA_VERSION) {
        return;
    }

    // The previous baseline and every transient were produced by a different
    // model and are unsafe to reinterpret. Settings and user alarm state stay.
    static const int old_keys[] = {
        PERSIST_KEY_HR_BUFFER,
        PERSIST_KEY_HR_BUF_IDX,
        PERSIST_KEY_HR_BUF_COUNT,
        PERSIST_KEY_TRIGGER_STREAK,
        PERSIST_KEY_VMC_EMA,
        PERSIST_KEY_DEBUG_HR,
        PERSIST_KEY_DEBUG_AVG,
        PERSIST_KEY_DEBUG_ACCEL,
        PERSIST_KEY_HR_BASELINE,
        PERSIST_KEY_NUDGE_PENDING,
        PERSIST_KEY_STREAK_START,
        PERSIST_KEY_DEBUG_LAST_TS,
        PERSIST_KEY_HRV_BASELINE,
        PERSIST_KEY_DEBUG_HRV,
        PERSIST_KEY_LAST_NUDGE,
        PERSIST_KEY_DEBUG_PHASE,
        PERSIST_KEY_WORKER_STATUS
    };
    for (uint8_t i = 0; i < sizeof(old_keys) / sizeof(old_keys[0]); ++i) {
        persist_delete(old_keys[i]);
    }
    persist_write_int(PERSIST_KEY_DETECTOR_SCHEMA,
                      DETECTOR_SCHEMA_VERSION);
    APP_LOG(APP_LOG_LEVEL_INFO,
            "NapBuster: detector schema migrated; recalibrating baseline");
}

static void prv_load_detector_state(void) {
    nap_detector_init(&s_detector, prv_get_sensitivity());
    if (persist_exists(PERSIST_KEY_HR_BASELINE)) {
        int baseline = persist_read_int(PERSIST_KEY_HR_BASELINE);
        if (baseline >= 40 && baseline <= 180) {
            nap_detector_restore_baseline(&s_detector, (uint16_t)baseline);
        }
    }
    s_saved_baseline = nap_detector_baseline_bpm(&s_detector);

    if (persist_exists(PERSIST_KEY_LAST_NUDGE)) {
        s_last_nudge_time = (time_t)persist_read_int(PERSIST_KEY_LAST_NUDGE);
    }
}

static void prv_save_baseline_if_changed(void) {
    uint16_t baseline = nap_detector_baseline_bpm(&s_detector);
    if (baseline == s_saved_baseline) {
        return;
    }
    if (baseline == 0) {
        persist_delete(PERSIST_KEY_HR_BASELINE);
    } else {
        persist_write_int(PERSIST_KEY_HR_BASELINE, (int)baseline);
    }
    s_saved_baseline = baseline;
}

static void prv_write_status(void) {
    int status = WORKER_STATUS_RUNNING;
    if (s_health_subscribed) status |= WORKER_STATUS_HEALTH;
    if (s_hr_capable) status |= WORKER_STATUS_HR_CAPABLE;
    if (s_hr_period != 0) status |= WORKER_STATUS_HR_ACTIVE;
    if (s_hrv_period != 0) status |= WORKER_STATUS_HRV_ACTIVE;
    if (s_last_motion_fresh) status |= WORKER_STATUS_MOTION_FRESH;
    if (nap_detector_baseline_bpm(&s_detector) != 0) {
        status |= WORKER_STATUS_BASELINE_READY;
    }
    if ((int)s_detector.phase != s_last_written_phase) {
        persist_write_int(PERSIST_KEY_DEBUG_PHASE, (int)s_detector.phase);
        s_last_written_phase = (int)s_detector.phase;
    }
    if (status != s_last_written_status) {
        persist_write_int(PERSIST_KEY_WORKER_STATUS, status);
        s_last_written_status = status;
    }
}

static void prv_write_rmssd(int rmssd) {
    if (rmssd == s_last_written_rmssd) return;
    persist_write_int(PERSIST_KEY_DEBUG_HRV, rmssd);
    s_last_written_rmssd = rmssd;
}

static void prv_write_analysis_telemetry(uint16_t smoothed_hr,
                                         const NapDetectorMotion *motion,
                                         int rmssd,
                                         time_t timestamp,
                                         bool force) {
    bool phase_changed =
        (int)s_detector.phase != s_last_written_phase;
    if (!force && !phase_changed && s_last_telemetry_time > 0 &&
        timestamp >= s_last_telemetry_time &&
        (timestamp - s_last_telemetry_time) < SECONDS_PER_MINUTE) {
        // Candidate analysis can run every 20 seconds. Once-per-minute
        // diagnostics remain useful without turning every probe into several
        // persistent-storage writes. Phase/status changes still write now.
        prv_write_status();
        return;
    }

    uint32_t evidence_minutes = s_detector.evidence_seconds / 60u;
    if (evidence_minutes > 255u) evidence_minutes = 255u;

    persist_write_int(PERSIST_KEY_TRIGGER_STREAK, (int)evidence_minutes);
    persist_write_int(PERSIST_KEY_DEBUG_HR, (int)smoothed_hr);
    persist_write_int(PERSIST_KEY_DEBUG_AVG,
                      (int)nap_detector_baseline_bpm(&s_detector));
    persist_write_int(PERSIST_KEY_DEBUG_ACCEL,
                      motion == NULL ? 0 : (int)motion->latest_vmc);
    persist_write_int(PERSIST_KEY_DEBUG_LAST_TS, (int)timestamp);
    prv_write_rmssd(rmssd);
    s_last_telemetry_time = timestamp;
    prv_write_status();
}

// Guard/pending-state helpers ------------------------------------------------

static bool prv_is_snoozed(void) {
    if (!persist_exists(PERSIST_KEY_SNOOZE_UNTIL)) return false;
    time_t until = (time_t)persist_read_int(PERSIST_KEY_SNOOZE_UNTIL);
    time_t now = time(NULL);
    if (until > now && (until - now) <= SNOOZE_MAX_SECS) return true;

    // Expired, implausibly far-future, or clock-warped values cannot disable
    // the detector indefinitely.
    persist_delete(PERSIST_KEY_SNOOZE_UNTIL);
    return false;
}

static bool prv_is_already_alarming(void) {
    if (!persist_exists(PERSIST_KEY_ALARMING) ||
        !persist_read_int(PERSIST_KEY_ALARMING)) {
        return false;
    }

    time_t now = time(NULL);
    time_t started = persist_exists(PERSIST_KEY_ALARM_START)
                         ? (time_t)persist_read_int(PERSIST_KEY_ALARM_START)
                         : 0;
    if (started > 0 && now >= started &&
        (now - started) <= ALARM_STALE_SECS) {
        return true;
    }

    persist_write_int(PERSIST_KEY_ALARMING, 0);
    persist_delete(PERSIST_KEY_ALARM_START);
    APP_LOG(APP_LOG_LEVEL_WARNING,
            "NapBuster: cleared stale alarm state");
    return false;
}

static bool prv_dismiss_cooldown_active(void) {
    if (!persist_exists(PERSIST_KEY_LAST_DISMISS)) return false;
    time_t last = (time_t)persist_read_int(PERSIST_KEY_LAST_DISMISS);
    time_t now = time(NULL);
    if (last > 0 && now >= last &&
        (now - last) < DISMISS_COOLDOWN_SECS) {
        return true;
    }
    persist_delete(PERSIST_KEY_LAST_DISMISS);
    return false;
}

static bool prv_nudge_cooldown_active(void) {
    time_t now = time(NULL);
    if (s_last_nudge_time > 0 && now >= s_last_nudge_time &&
        (now - s_last_nudge_time) < NUDGE_COOLDOWN_SECS) {
        return true;
    }
    if (s_last_nudge_time != 0) {
        s_last_nudge_time = 0;
        persist_delete(PERSIST_KEY_LAST_NUDGE);
    }
    return false;
}

static void prv_clear_ppi(void);
static void prv_set_sensor_periods(void);

static void prv_reset_episode(void) {
    nap_detector_reset_transient(&s_detector);
    s_last_accepted_hr_time = 0;
    s_last_motion_fresh = false;
    prv_clear_ppi();
    prv_write_rmssd(-1);
    persist_write_int(PERSIST_KEY_TRIGGER_STREAK, 0);
    persist_write_int(PERSIST_KEY_DEBUG_PHASE, NAP_DETECTOR_ARMED);
    s_last_written_phase = NAP_DETECTOR_ARMED;
}

static void prv_reconcile_foreground_state(void) {
    bool alarming = prv_is_already_alarming();
    bool paused = prv_is_snoozed() || prv_dismiss_cooldown_active();

    if (alarming) {
        s_observed_alarming = true;
        s_launch_pending = false;
        s_launch_pending_time = 0;
        if (s_detector.phase != NAP_DETECTOR_ARMED) {
            prv_reset_episode();
        }
        return;
    }

    // Persisted snooze/dismiss state is also an acknowledgement if its worker
    // message was lost. A true->false ALARMING transition covers foreground
    // exit paths that clear the flag without leaving either cooldown marker.
    if (s_observed_alarming || paused) {
        s_observed_alarming = false;
        s_launch_pending = false;
        s_launch_pending_time = 0;
        if (s_detector.phase != NAP_DETECTOR_ARMED) {
            prv_reset_episode();
        }
    }
}

static void prv_expire_pending_launches(void) {
    time_t now = time(NULL);
    if (s_launch_pending && prv_is_already_alarming()) {
        // The foreground established a bounded ALARMING session, so the
        // launch is acknowledged even though no explicit message is needed.
        s_launch_pending = false;
        s_launch_pending_time = 0;
    }
    bool alarm_expired = s_launch_pending &&
                         (s_launch_pending_time <= 0 ||
                          now < s_launch_pending_time ||
                          (now - s_launch_pending_time) >
                              LAUNCH_PENDING_SECS);
    if (alarm_expired) {
        s_launch_pending = false;
        s_launch_pending_time = 0;
        prv_reset_episode();
        APP_LOG(APP_LOG_LEVEL_WARNING,
                "NapBuster: unacknowledged alarm launch expired");
    }

    if (persist_exists(PERSIST_KEY_NUDGE_PENDING)) {
        bool stale_nudge = s_last_nudge_time <= 0 ||
                           now < s_last_nudge_time ||
                           (now - s_last_nudge_time) >
                               LAUNCH_PENDING_SECS;
        if (stale_nudge) {
            persist_delete(PERSIST_KEY_NUDGE_PENDING);
            prv_reset_episode();
            APP_LOG(APP_LOG_LEVEL_WARNING,
                    "NapBuster: unacknowledged nudge launch expired");
        }
    }
}

static bool prv_launch_allowed(bool nudge) {
    prv_expire_pending_launches();
    if (s_launch_pending || prv_is_already_alarming() ||
        prv_is_snoozed() || prv_dismiss_cooldown_active() ||
        !prv_get_enabled() || !prv_is_in_window()) {
        return false;
    }
    return !nudge || !prv_nudge_cooldown_active();
}

static bool prv_fire_nudge(void) {
    if (!prv_launch_allowed(true)) return false;

    s_last_nudge_time = time(NULL);
    persist_write_int(PERSIST_KEY_LAST_NUDGE, (int)s_last_nudge_time);
    persist_write_int(PERSIST_KEY_NUDGE_PENDING, 1);
    AppWorkerMessage message = { .data0 = WORKER_MSG_NAP_NUDGE };
    app_worker_send_message(WORKER_MSG_NAP_NUDGE, &message);
    // Deliver directly first if the app is already open. If it is closed the
    // message is dropped, then the persisted launch kind drives app startup.
    worker_launch_app();
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster: candidate nudge launched");
    return true;
}

static bool prv_fire_alarm(void) {
    if (!prv_launch_allowed(false)) return false;

    persist_delete(PERSIST_KEY_NUDGE_PENDING);
    s_launch_pending = true;
    s_launch_pending_time = time(NULL);
    AppWorkerMessage message = { .data0 = WORKER_MSG_SLEEP_DETECTED };
    app_worker_send_message(WORKER_MSG_SLEEP_DETECTED, &message);
    worker_launch_app();
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster: full alarm launched");
    return true;
}

// Health subscription and dynamic sensor requests ---------------------------

static bool prv_probe_hr_capable(void) {
    time_t now = time(NULL);
    HealthServiceAccessibilityMask raw =
        health_service_metric_accessible(HealthMetricHeartRateRawBPM,
                                         now, now);
    HealthServiceAccessibilityMask filtered =
        health_service_metric_accessible(HealthMetricHeartRateBPM,
                                         now - SECONDS_PER_HOUR, now);
    return ((raw | filtered) & HealthServiceAccessibilityMaskAvailable) != 0;
}

static void prv_subscribe_health(void) {
    if (s_health_subscribed) return;
    s_health_subscribed =
        health_service_events_subscribe(prv_health_event_handler, NULL);
    APP_LOG(s_health_subscribed ? APP_LOG_LEVEL_INFO : APP_LOG_LEVEL_WARNING,
            "NapBuster: HealthService subscribe=%d",
            (int)s_health_subscribed);
}

static void prv_unsubscribe_health(void) {
    if (!s_health_subscribed) return;
    health_service_events_unsubscribe();
    s_health_subscribed = false;
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster: HealthService unsubscribed");
}

static bool prv_request_due(time_t retry_after) {
    time_t now = time(NULL);
    if (retry_after == 0 || now >= retry_after) return true;
    // A large backward clock adjustment must not suppress retries forever.
    return (retry_after - now) > (SENSOR_RETRY_SECS + 60);
}

static void prv_set_hr_period(uint16_t desired) {
    if (desired == s_hr_period) return;
    if (desired != 0 && !prv_request_due(s_hr_retry_after)) return;

    bool ok = health_service_set_heart_rate_sample_period(desired);
    if (ok) {
        s_hr_period = desired;
        s_hr_retry_after = 0;
        if (desired != 0) s_hr_capable = true;
    } else {
        s_hr_retry_after = time(NULL) + SENSOR_RETRY_SECS;
    }
    APP_LOG(ok ? APP_LOG_LEVEL_INFO : APP_LOG_LEVEL_WARNING,
            "NapBuster: HR period=%u accepted=%d",
            (unsigned)desired, (int)ok);
}

static void prv_set_hrv_period(uint16_t desired) {
    if (desired == s_hrv_period) return;
    if (desired != 0 && !prv_request_due(s_hrv_retry_after)) return;

    bool ok = health_service_set_hrv_sample_period(desired);
    if (ok) {
        s_hrv_period = desired;
        s_hrv_retry_after = 0;
    } else {
        s_hrv_retry_after = time(NULL) + SENSOR_RETRY_SECS;
    }
    if (desired == 0 || s_hrv_period == 0) {
        prv_clear_ppi();
        prv_write_rmssd(-1);
    }
    APP_LOG(ok ? APP_LOG_LEVEL_INFO : APP_LOG_LEVEL_DEBUG,
            "NapBuster: HRV period=%u accepted=%d",
            (unsigned)desired, (int)ok);
}

static void prv_set_sensor_periods(void) {
    bool confirming = s_detector.phase != NAP_DETECTOR_ARMED;
    uint16_t desired_hr = 0;
    uint16_t desired_hrv = 0;

    if (s_window_active && s_health_subscribed) {
        // A successful period request is also the reliable fresh-install
        // hardware probe; accessibility may have no recent HR to report yet.
        desired_hr = confirming ? HR_CONFIRM_PERIOD_SECS
                                : HR_ARMED_PERIOD_SECS;
        desired_hrv = confirming && s_hr_capable
                          ? HRV_CONFIRM_PERIOD_SECS
                          : 0;
    }
    prv_set_hr_period(desired_hr);
    prv_set_hrv_period(desired_hrv);
    prv_write_status();
}

// Candidate-only, diagnostic HRV --------------------------------------------

static void prv_clear_ppi(void) {
    memset(s_ppi_ring, 0, sizeof(s_ppi_ring));
    s_ppi_count = 0;
    s_ppi_index = 0;
    s_last_ppi_time = 0;
}

static uint32_t prv_integer_sqrt(uint32_t value) {
    uint32_t result = 0;
    uint32_t bit = 1UL << 30;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

static int prv_current_rmssd(void) {
    time_t now = time(NULL);
    if (s_ppi_count < PPI_MIN_SAMPLES || s_last_ppi_time <= 0 ||
        now < s_last_ppi_time ||
        (now - s_last_ppi_time) > PPI_FRESH_SECS) {
        return -1;
    }

    uint8_t start = s_ppi_count == PPI_RING_SIZE ? s_ppi_index : 0;
    uint32_t squared_sum = 0;
    for (uint8_t i = 1; i < s_ppi_count; ++i) {
        int32_t current =
            s_ppi_ring[(uint8_t)((start + i) % PPI_RING_SIZE)];
        int32_t previous =
            s_ppi_ring[(uint8_t)((start + i - 1) % PPI_RING_SIZE)];
        int32_t difference = current - previous;
        squared_sum += (uint32_t)(difference * difference);
    }
    return (int)prv_integer_sqrt(squared_sum / (s_ppi_count - 1));
}

static void prv_ingest_ppi(uint16_t ppi_ms) {
    if (s_detector.phase == NAP_DETECTOR_ARMED || s_hrv_period == 0 ||
        ppi_ms < PPI_MIN_MS || ppi_ms > PPI_MAX_MS) {
        return;
    }

    time_t now = time(NULL);
    if (s_last_hr_bpm > 0 && s_last_hr_time > 0 && now >= s_last_hr_time &&
        (now - s_last_hr_time) <= 60) {
        int32_t expected = 60000 / s_last_hr_bpm;
        int32_t difference = (int32_t)ppi_ms - expected;
        if (difference < 0) difference = -difference;
        if (difference * 100 > expected * PPI_HR_TOLERANCE_PCT) return;
    }

    if (s_last_ppi_time > 0 &&
        (now < s_last_ppi_time ||
         (now - s_last_ppi_time) > PPI_CONTIGUOUS_GAP_SECS)) {
        prv_clear_ppi();
    }

    s_ppi_ring[s_ppi_index] = ppi_ms;
    s_ppi_index = (uint8_t)((s_ppi_index + 1) % PPI_RING_SIZE);
    if (s_ppi_count < PPI_RING_SIZE) ++s_ppi_count;
    s_last_ppi_time = now;
}

// Completed-minute motion summary -------------------------------------------

static bool prv_minute_overlaps_own_nudge(time_t record_start,
                                           time_t record_end) {
    if (s_last_nudge_time <= 0) return false;
    return s_last_nudge_time >= record_start &&
           s_last_nudge_time < record_end;
}

static NapDetectorMotion prv_get_motion_summary(time_t now) {
    NapDetectorMotion motion;
    memset(&motion, 0, sizeof(motion));

    HealthMinuteData records[MOTION_WINDOW_MINUTES];
    time_t completed_end = now - (now % SECONDS_PER_MINUTE);
    time_t returned_start =
        completed_end - MOTION_WINDOW_MINUTES * SECONDS_PER_MINUTE;
    time_t returned_end = completed_end;
    uint32_t count = health_service_get_minute_history(
        records, MOTION_WINDOW_MINUTES, &returned_start, &returned_end);
    if (count > MOTION_WINDOW_MINUTES) count = MOTION_WINDOW_MINUTES;

    uint32_t vmc_sum = 0;
    uint32_t steps_sum = 0;
    for (uint32_t i = 0; i < count; ++i) {
        time_t record_end =
            returned_start + (time_t)(i + 1) * SECONDS_PER_MINUTE;
        time_t record_start = record_end - SECONDS_PER_MINUTE;
        if (record_end > completed_end || records[i].is_invalid) continue;

        uint32_t vmc = records[i].vmc;
        uint32_t steps = records[i].steps;

        // The nudge is our actuator, not evidence that the user woke. Mask its
        // exact minute. The core accepts four valid minutes, so omitting this
        // record does not itself break continuity. Genuine movement in any
        // following minute still resets the detector. Step counts are kept:
        // the vibration can raise VMC, but it cannot make the wearer walk.
        bool own_nudge =
            prv_minute_overlaps_own_nudge(record_start, record_end);
        if (own_nudge) {
            steps_sum += steps;
            if ((uint32_t)record_end >= motion.latest_timestamp) {
                motion.latest_timestamp = (uint32_t)record_end;
                motion.latest_vmc = 0;
                motion.latest_steps =
                    steps > 65535u ? 65535u : (uint16_t)steps;
            }
            continue;
        }

        ++motion.valid_minutes;
        vmc_sum += vmc;
        steps_sum += steps;
        if (vmc > motion.peak_vmc) motion.peak_vmc = vmc;
        if (vmc < MOTION_QUIET_VMC_MAX && steps == 0) {
            ++motion.quiet_minutes;
        }
        if ((uint32_t)record_end >= motion.latest_timestamp) {
            motion.latest_timestamp = (uint32_t)record_end;
            motion.latest_vmc = vmc;
            motion.latest_steps =
                steps > 65535u ? 65535u : (uint16_t)steps;
        }
    }

    if (motion.valid_minutes != 0) {
        motion.mean_vmc = vmc_sum / motion.valid_minutes;
    }
    motion.window_steps =
        steps_sum > 65535u ? 65535u : (uint16_t)steps_sum;
    return motion;
}

// Detector integration ------------------------------------------------------

static void prv_analyze_heart_rate(int16_t heart_rate_bpm) {
    if (!s_health_subscribed || heart_rate_bpm <= 0) return;

    // Only an actual HealthEventHeartRateUpdate reaches here. There is no
    // fallback timer that can replay a stale filtered value as new evidence.
    time_t now = time(NULL);
    NapDetectorSample sample;
    sample.timestamp = (uint32_t)now;
    sample.heart_rate_bpm = heart_rate_bpm;
    sample.motion = prv_get_motion_summary(now);

    s_last_hr_bpm = heart_rate_bpm;
    s_last_hr_time = now;
    NapDetectorResult result = nap_detector_process(&s_detector, &sample);
    if (!result.sample_accepted) return;
    s_last_accepted_hr_time = now;
    s_last_motion_fresh = result.motion_fresh;
    prv_save_baseline_if_changed();

    if (!s_window_active && s_detector.phase != NAP_DETECTOR_ARMED) {
        // The lead-in may seed the slow awake baseline, but it must never carry
        // an episode/evidence streak across the actual guard boundary. ARMED
        // calibration history is retained; opening the window resets it once.
        prv_reset_episode();
    } else if (result.action == NAP_DETECTOR_ACTION_NUDGE) {
        if (!prv_fire_nudge()) prv_reset_episode();
    } else if (result.action == NAP_DETECTOR_ACTION_ALARM) {
        if (!prv_fire_alarm()) prv_reset_episode();
    }

    int rmssd = prv_current_rmssd();
    prv_write_analysis_telemetry(
        result.smoothed_hr_bpm, &sample.motion, rmssd, now,
        result.action != NAP_DETECTOR_ACTION_NONE);
    prv_set_sensor_periods();

    APP_LOG(APP_LOG_LEVEL_DEBUG,
            "NapBuster: hr=%d smooth=%u base=%u vmc=%u phase=%d ev=%lus",
            (int)heart_rate_bpm, (unsigned)result.smoothed_hr_bpm,
            (unsigned)nap_detector_baseline_bpm(&s_detector),
            (unsigned)sample.motion.latest_vmc, (int)s_detector.phase,
            (unsigned long)s_detector.evidence_seconds);
}

static void prv_check_tier2_sleep(void) {
    HealthActivityMask activity = health_service_peek_current_activities();
    if ((activity & HealthActivitySleep) ||
        (activity & HealthActivityRestfulSleep)) {
        if (!prv_fire_alarm()) {
            // Policy blocked the action; do not leave a consumed Tier-1 action
            // silently latched behind the foreground state.
            prv_reset_episode();
            prv_set_sensor_periods();
        }
    }
}

// Window/lifecycle state -----------------------------------------------------

static void prv_apply_window_state(void) {
    bool enabled = prv_get_enabled();
    bool in_window = enabled && prv_is_in_window();
    bool boundary_changed = in_window != s_window_active;

    if (boundary_changed) {
        s_window_active = in_window;
        s_launch_pending = false;
        s_launch_pending_time = 0;
        prv_reset_episode();
        APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster: guard window %s",
                in_window ? "opened" : "closed");
    }

    bool in_lead = enabled && s_hr_capable && !in_window &&
                   prv_is_in_lead_period();
    bool want_health = in_window || in_lead;
    if (want_health) prv_subscribe_health();

    prv_set_sensor_periods();
    if (!want_health) prv_unsubscribe_health();
    prv_write_status();

    if (in_window && boundary_changed && s_health_subscribed) {
        prv_check_tier2_sleep();
    }
}

static void prv_minute_tick(struct tm *tick_time, TimeUnits units_changed) {
    (void)tick_time;
    (void)units_changed;

    // This also ages out stale foreground ALARMING state even after a launch
    // was acknowledged, and recovers when an acknowledgement message is lost.
    prv_reconcile_foreground_state();
    prv_expire_pending_launches();
    time_t now = time(NULL);
    if (s_detector.phase != NAP_DETECTOR_ARMED &&
        (s_last_accepted_hr_time <= 0 || now < s_last_accepted_hr_time ||
         (now - s_last_accepted_hr_time) >
             (time_t)NAP_DETECTOR_MAX_GAP_SECONDS)) {
        // With no callbacks the portable core cannot observe the gap itself.
        // Bound candidate lifetime here so fast HR/HRV requests cannot stick.
        prv_reset_episode();
        APP_LOG(APP_LOG_LEVEL_WARNING,
                "NapBuster: stale candidate reset after HR event gap");
    }
    if (!s_hr_capable && prv_probe_hr_capable()) {
        s_hr_capable = true;
        APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster: heart rate became available");
    }
    prv_apply_window_state();
}

static void prv_health_event_handler(HealthEventType event, void *context) {
    (void)context;

    if (event == HealthEventHeartRateUpdate) {
        // The event can represent either raw or filtered HR changing. Filtered
        // BPM is itself an older moving average, so stamping it with `now`
        // would replay lagged data as fresh evidence. Analyze raw BPM only;
        // median smoothing and physiological gating live in the core.
        HealthValue current =
            health_service_peek_current_value(HealthMetricHeartRateRawBPM);
        if (current > 0) {
            if (!s_hr_capable) {
                s_hr_capable = true;
                prv_apply_window_state();
            }
            prv_analyze_heart_rate((int16_t)current);
        }
        return;
    }

    if (event == HealthEventHRVUpdate) {
        uint16_t ppi_ms = health_service_peek_hrv_ppi_ms();
        if (ppi_ms > 0) prv_ingest_ppi(ppi_ms);
        return;
    }

    // Pebble's classified sleep is a slower, all-platform fallback.
    if (s_window_active) prv_check_tier2_sleep();
}

static void prv_app_message_handler(uint16_t type, AppWorkerMessage *message) {
    (void)message;
    switch (type) {
        case APP_MSG_DISMISS:
        case APP_MSG_SNOOZE_10:
        case APP_MSG_SNOOZE_30:
            s_launch_pending = false;
            s_launch_pending_time = 0;
            s_observed_alarming = false;
            persist_delete(PERSIST_KEY_NUDGE_PENDING);
            prv_reset_episode();
            prv_set_sensor_periods();
            break;

        case APP_MSG_SETTINGS_CHANGED:
            s_launch_pending = false;
            s_launch_pending_time = 0;
            nap_detector_set_sensitivity(&s_detector,
                                         prv_get_sensitivity());
            prv_reset_episode();
            prv_apply_window_state();
            break;

        default:
            break;
    }
}

static void worker_init(void) {
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster detector worker starting");
    prv_migrate_detector_state();
    prv_load_detector_state();
    prv_clear_ppi();
    prv_write_rmssd(-1);
    persist_write_int(PERSIST_KEY_TRIGGER_STREAK, 0);
    persist_write_int(PERSIST_KEY_DEBUG_PHASE, NAP_DETECTOR_ARMED);

    app_worker_message_subscribe(prv_app_message_handler);
    s_hr_capable = prv_probe_hr_capable();
    prv_expire_pending_launches();
    prv_apply_window_state();
    // TickTimerService is aligned to real minute boundaries and is the
    // low-power worker primitive intended for ongoing schedule checks.
    tick_timer_service_subscribe(MINUTE_UNIT, prv_minute_tick);
}

static void worker_deinit(void) {
    APP_LOG(APP_LOG_LEVEL_INFO, "NapBuster detector worker stopping");
    tick_timer_service_unsubscribe();
    prv_set_hr_period(0);
    prv_set_hrv_period(0);
    prv_unsubscribe_health();
    prv_save_baseline_if_changed();
    persist_write_int(PERSIST_KEY_WORKER_STATUS, 0);
    app_worker_message_unsubscribe();
}

int main(void) {
    worker_init();
    worker_event_loop();
    worker_deinit();
    return 0;
}
