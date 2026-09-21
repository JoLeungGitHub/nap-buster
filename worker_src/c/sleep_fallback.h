#ifndef NAPBUSTER_SLEEP_FALLBACK_H
#define NAPBUSTER_SLEEP_FALLBACK_H

#include "nap_detector.h"

/* A recent, complete HR assessment or clear movement can contradict the OS
 * sleep label. Missing, warming-up, or stale data cannot veto the fallback. */
static inline bool nap_sleep_fallback_contradicted(
    const NapDetectorResult *result, bool smoothing_ready,
    uint32_t analyzed_at, uint32_t motion_at, uint32_t now) {
    if (analyzed_at == 0u || now < analyzed_at ||
        now - analyzed_at > NAP_DETECTOR_MAX_GAP_SECONDS) {
        return false;
    }
    bool fresh_motion = result->motion_fresh && now >= motion_at &&
        now - motion_at <= NAP_DETECTOR_MOTION_MAX_AGE_SECONDS;
    return (fresh_motion && result->movement) ||
           (smoothing_ready && result->hr_valid &&
            result->baseline_hr_bpm != 0u && !result->hr_full_drop);
}

/* Acknowledging an alarm is enough to suppress the same continuous OS sleep
 * report. Rearm only after the OS reports awake, without requiring a walk.
 * The caller marks handled after a successful alert and persists changes. */
static inline bool nap_sleep_fallback_update(bool *handled, bool asleep,
                                             bool acknowledged,
                                             bool contradicted) {
    if (!asleep) {
        *handled = false;
        return false;
    }
    if (acknowledged) *handled = true;
    return !*handled && !contradicted;
}

#endif
