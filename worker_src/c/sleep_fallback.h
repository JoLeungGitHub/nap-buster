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

/* Snooze expiry. Snooze used to re-ring unconditionally, which is alarm-clock
 * semantics: right for "wake me after a ten-minute power nap", wrong for "I'm
 * up, back off" -- and it rang with no evidence at all. It now asks the same
 * question as the OS-sleep fallback. A wearer who is still asleep still has a
 * dropped HR, so a power nap still ends in an alarm; fresh movement or a
 * recovered HR returns to guarding quietly instead. */
typedef enum {
    NAP_SNOOZE_CHECK_WAIT = 0, /* no fresh reading yet: keep the request */
    NAP_SNOOZE_CHECK_RESUME,   /* fresh evidence of wakefulness: guard quietly */
    NAP_SNOOZE_CHECK_RING,     /* still dropped, or no reading arrived in time */
    NAP_SNOOZE_CHECK_DROP      /* request stale or clock-warped: discard it */
} NapSnoozeCheck;

#define NAP_SNOOZE_CHECK_WAIT_SECONDS  180u
#define NAP_SNOOZE_CHECK_STALE_SECONDS 600u

static inline NapSnoozeCheck nap_snooze_check_decide(
    uint32_t requested_at, uint32_t now, const NapDetectorResult *result,
    bool smoothing_ready, uint32_t analyzed_at, uint32_t motion_at) {
    if (requested_at == 0u || now < requested_at ||
        now - requested_at > NAP_SNOOZE_CHECK_STALE_SECONDS) {
        return NAP_SNOOZE_CHECK_DROP;
    }
    bool fresh = analyzed_at != 0u && now >= analyzed_at &&
                 now - analyzed_at <= NAP_DETECTOR_MAX_GAP_SECONDS;
    if (!fresh) {
        /* Missing data cannot prove the wearer awake. Wait briefly for a
         * reading; past that, honour the snooze they asked for. */
        return now - requested_at < NAP_SNOOZE_CHECK_WAIT_SECONDS
                   ? NAP_SNOOZE_CHECK_WAIT : NAP_SNOOZE_CHECK_RING;
    }
    return nap_sleep_fallback_contradicted(result, smoothing_ready,
                                           analyzed_at, motion_at, now)
               ? NAP_SNOOZE_CHECK_RESUME : NAP_SNOOZE_CHECK_RING;
}

#endif
