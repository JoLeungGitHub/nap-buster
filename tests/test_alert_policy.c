#include "../src/common/alert_policy.h"
#include "../worker_src/c/sleep_fallback.h"

#include <stdio.h>

static unsigned int failures;
#define EXPECT(expression) do { \
    if (!(expression)) { \
        (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                      #expression); \
        failures++; \
    } \
} while (0)

static NapDetectorResult analyze(NapDetector *detector, uint32_t now,
                                 int16_t hr, uint32_t vmc) {
    NapDetectorSample sample = {0};
    sample.timestamp = now;
    sample.heart_rate_bpm = hr;
    sample.motion.latest_timestamp = now;
    sample.motion.latest_vmc = vmc;
    sample.motion.mean_vmc = vmc;
    sample.motion.peak_vmc = vmc;
    sample.motion.valid_minutes = 5u;
    sample.motion.quiet_minutes = vmc < 100u ? 5u : 0u;
    return nap_detector_process(detector, &sample);
}

static void test_dismiss_between_dispatch_and_delivery(void) {
    /* An alert was valid when queued. SELECT is pressed before another
     * message or worker launch arrives, including after foreground restart. */
    uint32_t queued_at = 1000u;
    uint32_t dismissed_at = queued_at + 1u;
    EXPECT(nap_alert_allowed(queued_at, true, true, 0u, 0u));
    EXPECT(!nap_alert_allowed(dismissed_at, true, true, 0u, dismissed_at));
    EXPECT(!nap_alert_allowed(dismissed_at + 1u, true, true, 0u, dismissed_at));
    EXPECT(!nap_alert_allowed(dismissed_at + 60u, true, true, 0u, dismissed_at));
    EXPECT(!nap_alert_allowed(dismissed_at + 599u, true, true, 0u, dismissed_at));
    EXPECT(nap_alert_allowed(dismissed_at + 600u, true, true, 0u, dismissed_at));

    EXPECT(!nap_alert_allowed(1001u, true, true, 1600u, 0u));
    EXPECT(nap_alert_allowed(1600u, true, true, 1600u, 0u));
    EXPECT(!nap_alert_allowed(1001u, false, true, 0u, 0u));
    EXPECT(!nap_alert_allowed(1001u, true, false, 0u, 0u));
    /* Invalid future timestamps must not suppress monitoring indefinitely. */
    EXPECT(nap_alert_allowed(1000u, true, true, 99999u, 2000u));
}

static void test_reported_awake_reading_contradicts_os_sleep(void) {
    for (int sensitivity = NAP_DETECTOR_SENSITIVE;
         sensitivity <= NAP_DETECTOR_CONSERVATIVE; ++sensitivity) {
        NapDetector detector;
        nap_detector_init(&detector, (NapDetectorSensitivity)sensitivity);
        nap_detector_restore_baseline(&detector, 88u);
        (void)analyze(&detector, 1000u, 86, 221u);
        (void)analyze(&detector, 1120u, 86, 221u);
        NapDetectorResult result = analyze(&detector, 1240u, 86, 221u);
        EXPECT(result.smoothed_hr_bpm == 86u);
        EXPECT(result.baseline_hr_bpm == 88u);
        EXPECT(result.action == NAP_DETECTOR_ACTION_NONE);
        EXPECT(result.phase == NAP_DETECTOR_ARMED);
        EXPECT(result.evidence_seconds == 0u);
        EXPECT(nap_sleep_fallback_contradicted(&result, true, 1240u,
                                               1240u, 1300u));
        bool handled = false;
        EXPECT(!nap_sleep_fallback_update(&handled, true, false, true));
        EXPECT(!handled); /* A later genuinely supporting reading may alert. */
    }

    /* Sitting completely still with the same awake-range HR must also veto
     * the fallback. No need to move to prove wakefulness to this policy. */
    NapDetector detector;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 88u);
    (void)analyze(&detector, 1000u, 86, 0u);
    (void)analyze(&detector, 1120u, 86, 0u);
    NapDetectorResult result = analyze(&detector, 1240u, 86, 0u);
    EXPECT(result.quiet);
    EXPECT(!result.movement);
    EXPECT(nap_sleep_fallback_contradicted(&result, true, 1240u, 1240u, 1240u));
}

static void test_same_os_sleep_episode_stays_acknowledged(void) {
    bool handled = false;
    EXPECT(nap_sleep_fallback_update(&handled, true, false, false));
    handled = true; /* Successful dispatch: one alarm for this OS episode. */
    EXPECT(!nap_sleep_fallback_update(&handled, true, true, false));
    /* Cooldown expires, but the same OS label is still not a new nap. */
    EXPECT(!nap_sleep_fallback_update(&handled, true, false, false));
    bool restored = handled; /* Worker restart restores the persisted latch. */
    EXPECT(!nap_sleep_fallback_update(&restored, true, false, false));
    EXPECT(!nap_sleep_fallback_update(&restored, false, false, false));
    EXPECT(!restored);
    EXPECT(nap_sleep_fallback_update(&restored, true, false, false));

    /* An acknowledgement recovered from persisted dismiss/snooze state
     * works even if the app-to-worker message was dropped. */
    handled = false;
    EXPECT(!nap_sleep_fallback_update(&handled, true, true, false));
    EXPECT(handled);
    EXPECT(!nap_sleep_fallback_update(&handled, true, false, false));
}

static void test_supporting_hr_and_missing_data_keep_fallback(void) {
    NapDetector detector;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 68u);
    (void)analyze(&detector, 1000u, 57, 0u);
    (void)analyze(&detector, 1120u, 57, 0u);
    NapDetectorResult result = analyze(&detector, 1240u, 57, 0u);
    EXPECT(result.hr_full_drop);
    EXPECT(!nap_sleep_fallback_contradicted(&result, true, 1240u, 1240u, 1240u));

    result = (NapDetectorResult){0}; /* Watch with no HR / unavailable data. */
    EXPECT(!nap_sleep_fallback_contradicted(&result, false, 0u, 0u, 1240u));
    result.hr_valid = true;
    result.baseline_hr_bpm = 88u;
    EXPECT(!nap_sleep_fallback_contradicted(&result, false, 1240u, 1240u, 1240u));
    result.baseline_hr_bpm = 0u; /* Calibration incomplete. */
    EXPECT(!nap_sleep_fallback_contradicted(&result, true, 1240u, 1240u, 1240u));
    result.baseline_hr_bpm = 88u;
    EXPECT(nap_sleep_fallback_contradicted(&result, true, 1240u, 1240u, 1480u));
    EXPECT(!nap_sleep_fallback_contradicted(&result, true, 1240u, 1240u, 1481u));
    EXPECT(!nap_sleep_fallback_contradicted(&result, true, 1240u, 1240u, 1200u));

    result.hr_valid = false;
    result.movement = true;
    result.motion_fresh = true;
    EXPECT(nap_sleep_fallback_contradicted(&result, false, 1240u, 1200u, 1320u));
    EXPECT(!nap_sleep_fallback_contradicted(&result, false, 1240u, 1200u, 1321u));
    result.motion_fresh = false;
    EXPECT(!nap_sleep_fallback_contradicted(&result, false, 1240u, 1240u, 1240u));
}

/* Reported: HR 71 against a 72 baseline, plainly awake, and a snooze re-rang
 * anyway because expiry never consulted the detector. */
static void test_snooze_expiry_while_awake_resumes_guard(void) {
    NapDetector detector;
    NapDetectorResult result = {0};
    uint32_t last = 0u;
    int i;

    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 72u);
    for (i = 0; i < 3; ++i) {
        last = 10000u + (uint32_t)i * 120u;
        result = analyze(&detector, last, 71, 20u);
    }
    EXPECT(!result.hr_full_drop);
    EXPECT(nap_snooze_check_decide(last, last + 30u, &result, true,
                                   last, last) == NAP_SNOOZE_CHECK_RESUME);
}

/* "Wake me after a ten-minute power nap" must keep working: someone still
 * asleep still has a dropped HR, so the snooze ends in an alarm as before. */
static void test_snooze_expiry_while_still_asleep_rings(void) {
    NapDetector detector;
    NapDetectorResult result = {0};
    uint32_t last = 0u;
    int i;

    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 72u);
    for (i = 0; i < 3; ++i) {
        last = 20000u + (uint32_t)i * 120u;
        result = analyze(&detector, last, 60, 20u);
    }
    EXPECT(result.hr_full_drop);
    EXPECT(nap_snooze_check_decide(last, last + 30u, &result, true,
                                   last, last) == NAP_SNOOZE_CHECK_RING);
}

/* Clear movement is enough on its own, even before HR smoothing is ready. */
static void test_snooze_expiry_with_fresh_movement_resumes_guard(void) {
    NapDetector detector;
    NapDetectorResult result;

    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 72u);
    result = analyze(&detector, 30000u, 60, 900u);
    EXPECT(result.movement);
    EXPECT(nap_snooze_check_decide(30000u, 30010u, &result, false,
                                   30000u, 30000u) ==
           NAP_SNOOZE_CHECK_RESUME);
}

/* Missing data cannot prove the wearer awake: wait briefly for a reading,
 * then honour the snooze they asked for. */
static void test_snooze_expiry_waits_briefly_for_a_reading(void) {
    NapDetectorResult none = {0};
    uint32_t requested = 40000u;

    EXPECT(nap_snooze_check_decide(requested, requested + 60u, &none, false,
                                   0u, 0u) == NAP_SNOOZE_CHECK_WAIT);
    EXPECT(nap_snooze_check_decide(requested,
                                   requested + NAP_SNOOZE_CHECK_WAIT_SECONDS,
                                   &none, false, 0u, 0u) ==
           NAP_SNOOZE_CHECK_RING);
    /* A reading older than the gap limit counts as missing too. */
    EXPECT(nap_snooze_check_decide(
               requested, requested + 30u, &none, false,
               requested - NAP_DETECTOR_MAX_GAP_SECONDS - 1u, 0u) ==
           NAP_SNOOZE_CHECK_WAIT);
}

static void test_stale_or_warped_snooze_request_is_dropped(void) {
    NapDetectorResult none = {0};
    uint32_t requested = 50000u;

    EXPECT(nap_snooze_check_decide(0u, requested, &none, false, 0u, 0u) ==
           NAP_SNOOZE_CHECK_DROP);
    EXPECT(nap_snooze_check_decide(requested, requested - 1u, &none, false,
                                   0u, 0u) == NAP_SNOOZE_CHECK_DROP);
    EXPECT(nap_snooze_check_decide(
               requested, requested + NAP_SNOOZE_CHECK_STALE_SECONDS + 1u,
               &none, false, 0u, 0u) == NAP_SNOOZE_CHECK_DROP);
}


int main(void) {
    test_snooze_expiry_while_awake_resumes_guard();
    test_snooze_expiry_while_still_asleep_rings();
    test_snooze_expiry_with_fresh_movement_resumes_guard();
    test_snooze_expiry_waits_briefly_for_a_reading();
    test_stale_or_warped_snooze_request_is_dropped();
    test_dismiss_between_dispatch_and_delivery();
    test_reported_awake_reading_contradicts_os_sleep();
    test_same_os_sleep_episode_stays_acknowledged();
    test_supporting_hr_and_missing_data_keep_fallback();
    if (failures != 0u) return 1;
    (void)puts("alert_policy: all regression tests passed");
    return 0;
}
