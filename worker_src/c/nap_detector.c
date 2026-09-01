#include "nap_detector.h"

#include <stddef.h>
#include <string.h>

#define HR_MIN_BPM 35u
#define HR_MAX_BPM 220u
#define BASELINE_MIN_BPM 40u
#define BASELINE_MAX_BPM 180u

/* Motion thresholds use Pebble HealthMinuteData VMC units. */
#define QUIET_LATEST_VMC_MAX 100u
#define QUIET_MEAN_VMC_MAX 120u
#define QUIET_WINDOW_STEPS_MAX 8u

#define MOVEMENT_LATEST_VMC_MIN 180u
#define MOVEMENT_MEAN_VMC_MIN 160u
#define MOVEMENT_PEAK_VMC_MIN 500u
#define MOVEMENT_LATEST_STEPS_MIN 3u
#define MOVEMENT_WINDOW_STEPS_MIN 20u

#define EXERCISE_LATEST_VMC_MIN 500u
#define EXERCISE_MEAN_VMC_MIN 350u
#define EXERCISE_PEAK_VMC_MIN 1000u
#define EXERCISE_LATEST_STEPS_MIN 20u
#define EXERCISE_WINDOW_STEPS_MIN 100u

#define BASELINE_SCALE 256
#define BASELINE_UPDATE_DIVISOR 64
#define BASELINE_CLUSTER_MIN_BPM 6u
#define BASELINE_CLUSTER_PERCENT 8u
#define BASELINE_UPWARD_CAP_PERCENT 10

static NapDetectorSensitivity sanitize_sensitivity(
    NapDetectorSensitivity sensitivity) {
    if (sensitivity != NAP_DETECTOR_SENSITIVE &&
        sensitivity != NAP_DETECTOR_BALANCED &&
        sensitivity != NAP_DETECTOR_CONSERVATIVE) {
        return NAP_DETECTOR_BALANCED;
    }
    return sensitivity;
}

static uint16_t median3(uint16_t a, uint16_t b, uint16_t c) {
    if (a > b) {
        uint16_t temporary = a;
        a = b;
        b = temporary;
    }
    if (b > c) {
        uint16_t temporary = b;
        b = c;
        c = temporary;
    }
    if (a > b) {
        b = a;
    }
    return b;
}

static uint16_t baseline_bpm(const NapDetector *detector) {
    if (detector == NULL || detector->baseline_q8 <= 0) {
        return 0u;
    }
    return (uint16_t)((detector->baseline_q8 + (BASELINE_SCALE / 2)) /
                      BASELINE_SCALE);
}

static void reset_episode(NapDetector *detector) {
    detector->phase = NAP_DETECTOR_ARMED;
    detector->evidence_seconds = 0u;
    detector->previous_positive = false;
    detector->last_positive_timestamp = 0u;
    detector->has_last_positive = false;
    detector->nudge_sent = false;
    detector->alarm_sent = false;
}

static void reset_hr_history(NapDetector *detector) {
    memset(detector->hr_window, 0, sizeof(detector->hr_window));
    detector->hr_count = 0u;
    detector->hr_index = 0u;
    detector->previous_smoothed_hr = 0u;
    detector->has_previous_smoothed_hr = false;
}

static void clear_seed_history(NapDetector *detector) {
    memset(detector->seed_hr, 0, sizeof(detector->seed_hr));
    detector->seed_count = 0u;
}

static bool motion_is_fresh(const NapDetectorSample *sample) {
    if (sample->motion.latest_timestamp > sample->timestamp) {
        return false;
    }
    return (sample->timestamp - sample->motion.latest_timestamp) <=
           NAP_DETECTOR_MOTION_MAX_AGE_SECONDS;
}

static bool motion_is_exercise(const NapDetectorMotion *motion) {
    return motion->latest_vmc >= EXERCISE_LATEST_VMC_MIN ||
           motion->mean_vmc >= EXERCISE_MEAN_VMC_MIN ||
           motion->peak_vmc >= EXERCISE_PEAK_VMC_MIN ||
           motion->latest_steps >= EXERCISE_LATEST_STEPS_MIN ||
           motion->window_steps >= EXERCISE_WINDOW_STEPS_MIN;
}

static bool motion_is_clear_movement(const NapDetectorMotion *motion) {
    return motion->latest_vmc >= MOVEMENT_LATEST_VMC_MIN ||
           motion->mean_vmc >= MOVEMENT_MEAN_VMC_MIN ||
           motion->peak_vmc >= MOVEMENT_PEAK_VMC_MIN ||
           motion->latest_steps >= MOVEMENT_LATEST_STEPS_MIN ||
           motion->window_steps >= MOVEMENT_WINDOW_STEPS_MIN;
}

static bool motion_is_quiet(const NapDetectorMotion *motion) {
    return motion->valid_minutes >= NAP_DETECTOR_MIN_VALID_MOTION_MINUTES &&
           motion->quiet_minutes >= 4u &&
           motion->latest_vmc < QUIET_LATEST_VMC_MAX &&
           motion->mean_vmc < QUIET_MEAN_VMC_MAX &&
           motion->latest_steps == 0u &&
           motion->window_steps <= QUIET_WINDOW_STEPS_MAX;
}

static uint8_t full_drop_percent(NapDetectorSensitivity sensitivity) {
    static const uint8_t thresholds[] = {8u, 12u, 16u};
    return thresholds[(unsigned int)sanitize_sensitivity(sensitivity)];
}

static uint8_t soft_drop_percent(NapDetectorSensitivity sensitivity) {
    static const uint8_t thresholds[] = {4u, 6u, 8u};
    return thresholds[(unsigned int)sanitize_sensitivity(sensitivity)];
}

static bool meets_drop(uint16_t smoothed_hr,
                       uint16_t baseline,
                       uint8_t drop_percent) {
    return (uint32_t)smoothed_hr * 100u <=
           (uint32_t)baseline * (uint32_t)(100u - drop_percent);
}

static uint16_t push_hr(NapDetector *detector, uint16_t heart_rate_bpm) {
    detector->hr_window[detector->hr_index] = heart_rate_bpm;
    detector->hr_index =
        (uint8_t)((detector->hr_index + 1u) % NAP_DETECTOR_HR_WINDOW_SIZE);
    if (detector->hr_count < NAP_DETECTOR_HR_WINDOW_SIZE) {
        detector->hr_count++;
    }

    if (detector->hr_count < NAP_DETECTOR_HR_WINDOW_SIZE) {
        return heart_rate_bpm;
    }
    return median3(detector->hr_window[0], detector->hr_window[1],
                   detector->hr_window[2]);
}

static void sort3(uint16_t values[3]) {
    uint16_t temporary;
    if (values[0] > values[1]) {
        temporary = values[0];
        values[0] = values[1];
        values[1] = temporary;
    }
    if (values[1] > values[2]) {
        temporary = values[1];
        values[1] = values[2];
        values[2] = temporary;
    }
    if (values[0] > values[1]) {
        temporary = values[0];
        values[0] = values[1];
        values[1] = temporary;
    }
}

static void maybe_seed_baseline(NapDetector *detector,
                                const NapDetectorSample *sample,
                                bool motion_fresh,
                                bool exercise,
                                bool movement,
                                bool quiet,
                                bool hr_valid) {
    uint16_t values[3];
    uint16_t range;
    uint16_t lower_gap;
    uint16_t upper_gap;
    bool stable_cluster;

    if (detector->baseline_q8 != 0 || !hr_valid || !motion_fresh || exercise ||
        movement || !quiet ||
        sample->motion.valid_minutes <
            NAP_DETECTOR_MIN_VALID_MOTION_MINUTES) {
        return;
    }

    detector->seed_hr[detector->seed_count] =
        (uint16_t)sample->heart_rate_bpm;
    detector->seed_count++;

    if (detector->seed_count == NAP_DETECTOR_HR_WINDOW_SIZE) {
        values[0] = detector->seed_hr[0];
        values[1] = detector->seed_hr[1];
        values[2] = detector->seed_hr[2];
        sort3(values);
        range = (uint16_t)(values[2] - values[0]);
        stable_cluster =
            range <= BASELINE_CLUSTER_MIN_BPM ||
            (uint32_t)range * 100u <=
                (uint32_t)values[1] * BASELINE_CLUSTER_PERCENT;

        if (stable_cluster) {
            detector->baseline_q8 = (int32_t)values[1] * BASELINE_SCALE;
            clear_seed_history(detector);
            return;
        }

        /* Keep the closest pair so one plausible-but-wrong HR sample does
         * not force calibration to restart from zero. */
        lower_gap = (uint16_t)(values[1] - values[0]);
        upper_gap = (uint16_t)(values[2] - values[1]);
        if (lower_gap <= upper_gap) {
            detector->seed_hr[0] = values[0];
            detector->seed_hr[1] = values[1];
        } else {
            detector->seed_hr[0] = values[1];
            detector->seed_hr[1] = values[2];
        }
        detector->seed_hr[2] = 0u;
        detector->seed_count = 2u;
    }
}

static void update_quiet_baseline(NapDetector *detector,
                                  uint16_t smoothed_hr) {
    int32_t target_q8 = (int32_t)smoothed_hr * BASELINE_SCALE;
    int32_t upward_cap_q8 =
        detector->baseline_q8 +
        (detector->baseline_q8 * BASELINE_UPWARD_CAP_PERCENT) / 100;

    if (target_q8 > upward_cap_q8) {
        target_q8 = upward_cap_q8;
    }
    detector->baseline_q8 +=
        (target_q8 - detector->baseline_q8) / BASELINE_UPDATE_DIVISOR;

    if (detector->baseline_q8 < (int32_t)BASELINE_MIN_BPM * BASELINE_SCALE) {
        detector->baseline_q8 = (int32_t)BASELINE_MIN_BPM * BASELINE_SCALE;
    } else if (detector->baseline_q8 >
               (int32_t)BASELINE_MAX_BPM * BASELINE_SCALE) {
        detector->baseline_q8 = (int32_t)BASELINE_MAX_BPM * BASELINE_SCALE;
    }
}

static void decay_ambiguous_evidence(NapDetector *detector,
                                     uint32_t elapsed_seconds) {
    uint32_t decay = elapsed_seconds / 2u;
    if (decay == 0u && elapsed_seconds != 0u) {
        decay = 1u;
    }

    if (decay >= detector->evidence_seconds) {
        reset_episode(detector);
    } else {
        detector->evidence_seconds -= decay;
        detector->previous_positive = false;
    }
}

static NapDetectorResult current_result(const NapDetector *detector) {
    NapDetectorResult result;
    memset(&result, 0, sizeof(result));
    result.phase = detector != NULL ? detector->phase : NAP_DETECTOR_ARMED;
    result.baseline_hr_bpm = baseline_bpm(detector);
    result.evidence_seconds =
        detector != NULL ? detector->evidence_seconds : 0u;
    result.request_fast_sampling =
        detector != NULL && detector->phase != NAP_DETECTOR_ARMED;
    return result;
}

void nap_detector_init(NapDetector *detector,
                       NapDetectorSensitivity sensitivity) {
    if (detector == NULL) {
        return;
    }
    memset(detector, 0, sizeof(*detector));
    detector->phase = NAP_DETECTOR_ARMED;
    detector->sensitivity = sanitize_sensitivity(sensitivity);
}

void nap_detector_reset_transient(NapDetector *detector) {
    int32_t saved_baseline_q8;
    NapDetectorSensitivity saved_sensitivity;

    if (detector == NULL) {
        return;
    }

    saved_baseline_q8 = detector->baseline_q8;
    saved_sensitivity = sanitize_sensitivity(detector->sensitivity);
    memset(detector, 0, sizeof(*detector));
    detector->phase = NAP_DETECTOR_ARMED;
    detector->sensitivity = saved_sensitivity;
    detector->baseline_q8 = saved_baseline_q8;
}

void nap_detector_set_sensitivity(NapDetector *detector,
                                  NapDetectorSensitivity sensitivity) {
    if (detector == NULL) {
        return;
    }
    detector->sensitivity = sanitize_sensitivity(sensitivity);
}

void nap_detector_restore_baseline(NapDetector *detector, uint16_t bpm) {
    if (detector == NULL) {
        return;
    }

    if (bpm == 0u) {
        detector->baseline_q8 = 0;
        detector->seed_count = 0u;
        memset(detector->seed_hr, 0, sizeof(detector->seed_hr));
        return;
    }

    if (bpm < BASELINE_MIN_BPM) {
        bpm = BASELINE_MIN_BPM;
    } else if (bpm > BASELINE_MAX_BPM) {
        bpm = BASELINE_MAX_BPM;
    }
    detector->baseline_q8 = (int32_t)bpm * BASELINE_SCALE;
    detector->seed_count = 0u;
    memset(detector->seed_hr, 0, sizeof(detector->seed_hr));
}

uint16_t nap_detector_baseline_bpm(const NapDetector *detector) {
    return baseline_bpm(detector);
}

NapDetectorResult nap_detector_process(NapDetector *detector,
                                       const NapDetectorSample *sample) {
    NapDetectorResult result;
    uint32_t elapsed_seconds = 0u;
    uint16_t smoothed_hr = 0u;
    uint16_t baseline;
    bool smoothing_ready = false;
    bool phase_was_armed;
    bool baseline_was_ready;

    if (detector == NULL || sample == NULL) {
        return current_result(detector);
    }

    result = current_result(detector);

    if (detector->has_last_sample) {
        if (sample->timestamp == detector->last_sample_timestamp) {
            return result;
        }
        if (sample->timestamp < detector->last_sample_timestamp) {
            /* Wall-clock corrections must not lock out every sample until the
             * old timestamp is reached again.  Start timing and smoothing
             * afresh, retaining only the calibrated baseline/sensitivity. */
            nap_detector_reset_transient(detector);
        } else {
            elapsed_seconds =
                sample->timestamp - detector->last_sample_timestamp;
            if (elapsed_seconds <
                NAP_DETECTOR_MIN_SAMPLE_INTERVAL_SECONDS) {
                return result;
            }
            if (elapsed_seconds > NAP_DETECTOR_MAX_GAP_SECONDS) {
                reset_episode(detector);
                reset_hr_history(detector);
                clear_seed_history(detector);
            }
        }
    }

    /* Accepted missing/ambiguous callbacks cannot refresh a stale episode.
     * Continuity is bounded by the last genuinely positive observation. */
    if (detector->phase != NAP_DETECTOR_ARMED &&
        detector->has_last_positive &&
        sample->timestamp - detector->last_positive_timestamp >
            NAP_DETECTOR_MAX_GAP_SECONDS) {
        reset_episode(detector);
    }

    phase_was_armed = detector->phase == NAP_DETECTOR_ARMED;
    baseline_was_ready = detector->baseline_q8 != 0;
    result.sample_accepted = true;
    result.motion_fresh = motion_is_fresh(sample);
    if (result.motion_fresh && sample->motion.valid_minutes != 0u) {
        result.exercise = motion_is_exercise(&sample->motion);
        result.movement = motion_is_clear_movement(&sample->motion);
        result.quiet = motion_is_quiet(&sample->motion);
    }

    if (!result.motion_fresh || !result.quiet || result.movement ||
        result.exercise) {
        clear_seed_history(detector);
    }

    result.hr_valid = sample->heart_rate_bpm >= (int16_t)HR_MIN_BPM &&
                      sample->heart_rate_bpm <= (int16_t)HR_MAX_BPM;
    if (!result.hr_valid) {
        clear_seed_history(detector);
    }
    if (result.hr_valid) {
        smoothed_hr = push_hr(detector, (uint16_t)sample->heart_rate_bpm);
        smoothing_ready =
            detector->hr_count == NAP_DETECTOR_HR_WINDOW_SIZE;
        result.smoothed_hr_bpm = smoothed_hr;
    }

    maybe_seed_baseline(detector, sample, result.motion_fresh,
                        result.exercise, result.movement, result.quiet,
                        result.hr_valid);
    baseline = baseline_bpm(detector);

    if (result.hr_valid && smoothing_ready && baseline != 0u) {
        result.hr_full_drop =
            meets_drop(smoothed_hr, baseline,
                       full_drop_percent(detector->sensitivity));
        result.hr_soft_drop =
            meets_drop(smoothed_hr, baseline,
                       soft_drop_percent(detector->sensitivity));
        result.hr_falling = detector->has_previous_smoothed_hr &&
                            smoothed_hr < detector->previous_smoothed_hr;
        /* Falling is required to enter on only a soft drop.  Once an episode
         * is underway, a low plateau is sustained evidence rather than a
         * negative sample; otherwise the normal onset plateau could never
         * accumulate enough time to alert. */
        result.positive = result.quiet && !result.movement &&
                          (result.hr_full_drop ||
                           (result.hr_soft_drop &&
                            (detector->phase != NAP_DETECTOR_ARMED ||
                             result.hr_falling)));
    }

    /* A quiet, awake-range HR is the only steady-state calibration signal.
     * The soft boundary excludes relaxation/doze values, and phase_was_armed
     * freezes the baseline for the entire candidate episode. */
    if (phase_was_armed && baseline_was_ready && result.motion_fresh &&
        result.quiet && !result.movement && !result.exercise &&
        result.hr_valid && smoothing_ready && !result.hr_soft_drop) {
        update_quiet_baseline(detector, smoothed_hr);
        baseline = baseline_bpm(detector);
    }

    if (result.movement) {
        reset_episode(detector);
    } else if (result.positive) {
        if (detector->phase == NAP_DETECTOR_ARMED) {
            detector->phase = NAP_DETECTOR_CANDIDATE;
            detector->evidence_seconds = 0u;
        } else if (detector->previous_positive) {
            detector->evidence_seconds += elapsed_seconds;
        }

        detector->previous_positive = true;
        detector->last_positive_timestamp = sample->timestamp;
        detector->has_last_positive = true;

        if (detector->evidence_seconds >= NAP_DETECTOR_ALARM_SECONDS) {
            if (result.hr_full_drop) {
                detector->phase = NAP_DETECTOR_NUDGED;
                if (!detector->alarm_sent) {
                    result.action = NAP_DETECTOR_ACTION_ALARM;
                    detector->alarm_sent = true;
                }
            } else {
                /* A mild relaxation drop can justify a nudge, but never a
                 * full alarm.  End it at the alarm horizon so fast sensing
                 * and a stale NUDGED phase cannot continue indefinitely. */
                reset_episode(detector);
            }
        } else if (detector->evidence_seconds >=
                   NAP_DETECTOR_NUDGE_SECONDS) {
            detector->phase = NAP_DETECTOR_NUDGED;
            if (!detector->nudge_sent) {
                result.action = NAP_DETECTOR_ACTION_NUDGE;
                detector->nudge_sent = true;
            }
        }
    } else {
        detector->previous_positive = false;
        if (detector->phase != NAP_DETECTOR_ARMED && result.motion_fresh &&
            result.quiet && result.hr_valid && smoothing_ready) {
            decay_ambiguous_evidence(detector, elapsed_seconds);
        }
    }

    if (result.hr_valid && smoothing_ready) {
        detector->previous_smoothed_hr = smoothed_hr;
        detector->has_previous_smoothed_hr = true;
    }

    detector->last_sample_timestamp = sample->timestamp;
    detector->has_last_sample = true;

    result.phase = detector->phase;
    result.baseline_hr_bpm = baseline;
    result.evidence_seconds = detector->evidence_seconds;
    result.request_fast_sampling = detector->phase != NAP_DETECTOR_ARMED;
    return result;
}
