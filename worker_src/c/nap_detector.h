#ifndef NAP_DETECTOR_H
#define NAP_DETECTOR_H

/*
 * Portable doze-onset detector.
 *
 * This module deliberately has no Pebble SDK dependencies.  The platform
 * adapter supplies one timestamped heart-rate reading and a summary of the
 * newest five motion minutes on every call to nap_detector_process().
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NAP_DETECTOR_HR_WINDOW_SIZE          3u
#define NAP_DETECTOR_MOTION_WINDOW_MINUTES   5u
#define NAP_DETECTOR_MIN_VALID_MOTION_MINUTES 4u
#define NAP_DETECTOR_MOTION_MAX_AGE_SECONDS 120u
#define NAP_DETECTOR_MIN_SAMPLE_INTERVAL_SECONDS 20u
#define NAP_DETECTOR_MAX_GAP_SECONDS        240u
#define NAP_DETECTOR_NUDGE_SECONDS          240u
#define NAP_DETECTOR_ALARM_SECONDS          600u

typedef enum {
    NAP_DETECTOR_SENSITIVE = 0,
    NAP_DETECTOR_BALANCED = 1,
    NAP_DETECTOR_CONSERVATIVE = 2
} NapDetectorSensitivity;

typedef enum {
    NAP_DETECTOR_ARMED = 0,
    NAP_DETECTOR_CANDIDATE = 1,
    NAP_DETECTOR_NUDGED = 2
} NapDetectorPhase;

typedef enum {
    NAP_DETECTOR_ACTION_NONE = 0,
    NAP_DETECTOR_ACTION_NUDGE = 1,
    NAP_DETECTOR_ACTION_ALARM = 2
} NapDetectorAction;

typedef struct {
    /* Timestamp of the newest minute included in this summary. */
    uint32_t latest_timestamp;
    uint32_t latest_vmc;
    uint32_t mean_vmc;
    uint32_t peak_vmc;
    uint16_t latest_steps;
    uint16_t window_steps;
    uint8_t valid_minutes;
    uint8_t quiet_minutes;
} NapDetectorMotion;

typedef struct {
    uint32_t timestamp;
    int16_t heart_rate_bpm;
    NapDetectorMotion motion;
} NapDetectorSample;

typedef struct {
    NapDetectorAction action;
    NapDetectorPhase phase;

    /* Classification details for telemetry and adapter decisions. */
    bool sample_accepted;
    bool motion_fresh;
    bool quiet;
    bool movement;
    bool exercise;
    bool hr_valid;
    bool hr_full_drop;
    bool hr_soft_drop;
    bool hr_falling;
    bool positive;

    uint16_t smoothed_hr_bpm;
    uint16_t baseline_hr_bpm;
    uint32_t evidence_seconds;
    bool request_fast_sampling;
} NapDetectorResult;

/*
 * The state is public so embedded callers can allocate it statically and, if
 * desired, persist it as individual fields.  Callers should otherwise treat
 * the fields as private implementation details.
 */
typedef struct {
    NapDetectorPhase phase;
    NapDetectorSensitivity sensitivity;

    uint16_t hr_window[NAP_DETECTOR_HR_WINDOW_SIZE];
    uint16_t seed_hr[NAP_DETECTOR_HR_WINDOW_SIZE];
    uint8_t hr_count;
    uint8_t hr_index;
    uint8_t seed_count;

    int32_t baseline_q8;
    uint16_t previous_smoothed_hr;
    uint32_t last_sample_timestamp;
    uint32_t last_positive_timestamp;
    uint32_t evidence_seconds;

    bool has_previous_smoothed_hr;
    bool has_last_sample;
    bool has_last_positive;
    bool previous_positive;
    bool nudge_sent;
    bool alarm_sent;
} NapDetector;

void nap_detector_init(NapDetector *detector,
                       NapDetectorSensitivity sensitivity);

/* Clear a possible doze episode while retaining a calibrated HR baseline. */
void nap_detector_reset_transient(NapDetector *detector);

void nap_detector_set_sensitivity(NapDetector *detector,
                                  NapDetectorSensitivity sensitivity);

/* bpm == 0 clears calibration; other values are physiologically clamped. */
void nap_detector_restore_baseline(NapDetector *detector, uint16_t bpm);

uint16_t nap_detector_baseline_bpm(const NapDetector *detector);

NapDetectorResult nap_detector_process(NapDetector *detector,
                                       const NapDetectorSample *sample);

#ifdef __cplusplus
}
#endif

#endif /* NAP_DETECTOR_H */
