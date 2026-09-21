#ifndef NAPBUSTER_ALERT_POLICY_H
#define NAPBUSTER_ALERT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define NAP_DISMISS_COOLDOWN_SECONDS 600u
#define NAP_MAX_SNOOZE_SECONDS 7200u

/* Used at both dispatch and delivery: queued worker messages must not undo
 * a dismissal, snooze, or settings change made after they were sent. */
static inline bool nap_dismiss_cooldown_active(uint32_t now,
                                               uint32_t dismissed_at) {
    return dismissed_at != 0u && now >= dismissed_at &&
           now - dismissed_at < NAP_DISMISS_COOLDOWN_SECONDS;
}

static inline bool nap_alert_allowed(uint32_t now, bool enabled,
                                     bool in_window, uint32_t snooze_until,
                                     uint32_t dismissed_at) {
    bool snoozed = snooze_until > now &&
                   snooze_until - now <= NAP_MAX_SNOOZE_SECONDS;
    return enabled && in_window && !snoozed &&
           !nap_dismiss_cooldown_active(now, dismissed_at);
}

#endif
