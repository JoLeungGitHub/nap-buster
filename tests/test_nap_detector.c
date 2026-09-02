#include "../worker_src/c/nap_detector.h"

#include <stdio.h>

static unsigned int failures;

#define EXPECT(expression)                                                    \
    do {                                                                      \
        if (!(expression)) {                                                  \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                          #expression);                                       \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static NapDetectorMotion quiet_motion(uint32_t timestamp) {
    NapDetectorMotion motion = {0};
    motion.latest_timestamp = timestamp;
    motion.latest_vmc = 20u;
    motion.mean_vmc = 30u;
    motion.peak_vmc = 70u;
    motion.valid_minutes = 5u;
    motion.quiet_minutes = 5u;
    return motion;
}

static NapDetectorMotion movement_motion(uint32_t timestamp) {
    NapDetectorMotion motion = {0};
    motion.latest_timestamp = timestamp;
    motion.latest_vmc = 220u;
    motion.mean_vmc = 130u;
    motion.peak_vmc = 300u;
    motion.latest_steps = 4u;
    motion.window_steps = 12u;
    motion.valid_minutes = 5u;
    motion.quiet_minutes = 2u;
    return motion;
}

static NapDetectorMotion exercise_motion(uint32_t timestamp) {
    NapDetectorMotion motion = {0};
    motion.latest_timestamp = timestamp;
    motion.latest_vmc = 650u;
    motion.mean_vmc = 420u;
    motion.peak_vmc = 1200u;
    motion.latest_steps = 24u;
    motion.window_steps = 120u;
    motion.valid_minutes = 5u;
    motion.quiet_minutes = 0u;
    return motion;
}

static NapDetectorResult feed(NapDetector *detector,
                              uint32_t timestamp,
                              int16_t heart_rate_bpm,
                              NapDetectorMotion motion) {
    NapDetectorSample sample;
    sample.timestamp = timestamp;
    sample.heart_rate_bpm = heart_rate_bpm;
    sample.motion = motion;
    return nap_detector_process(detector, &sample);
}

static NapDetectorResult feed_quiet(NapDetector *detector,
                                    uint32_t timestamp,
                                    int16_t heart_rate_bpm) {
    return feed(detector, timestamp, heart_rate_bpm,
                quiet_motion(timestamp));
}

/* Returns the timestamp of the first positive candidate sample. */
static uint32_t prepare_balanced_candidate(NapDetector *detector,
                                           uint32_t start) {
    NapDetectorResult result;
    nap_detector_init(detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(detector, 80u);
    (void)feed_quiet(detector, start, 80);
    (void)feed_quiet(detector, start + 120u, 80);
    (void)feed_quiet(detector, start + 240u, 80);
    (void)feed_quiet(detector, start + 360u, 70);
    result = feed_quiet(detector, start + 480u, 70);
    EXPECT(result.positive);
    EXPECT(result.phase == NAP_DETECTOR_CANDIDATE);
    EXPECT(result.evidence_seconds == 0u);
    return start + 480u;
}

static void test_quiet_awake_has_no_action(void) {
    NapDetector detector;
    NapDetectorResult result = {0};
    uint32_t timestamp = 1000u;
    unsigned int index;

    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 80u);
    for (index = 0u; index < 12u; index++) {
        int16_t hr = (index % 3u == 0u) ? 78 : 80;
        result = feed_quiet(&detector, timestamp, hr);
        EXPECT(result.action == NAP_DETECTOR_ACTION_NONE);
        timestamp += 120u;
    }

    EXPECT(result.phase == NAP_DETECTOR_ARMED);
    EXPECT(result.evidence_seconds == 0u);
    EXPECT(!result.request_fast_sampling);
}

static void test_real_doze_nudges_and_alarms_at_exact_boundaries(void) {
    NapDetector detector;
    NapDetectorResult result;
    uint32_t candidate_time = prepare_balanced_candidate(&detector, 2000u);

    result = feed_quiet(&detector, candidate_time + 99u, 70);
    EXPECT(result.evidence_seconds == 99u);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NONE);

    result = feed_quiet(&detector, candidate_time + 120u, 70);
    EXPECT(result.evidence_seconds == NAP_DETECTOR_NUDGE_SECONDS);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NUDGE);
    EXPECT(result.phase == NAP_DETECTOR_NUDGED);
    EXPECT(result.request_fast_sampling);

    result = feed_quiet(&detector, candidate_time + 279u, 70);
    EXPECT(result.evidence_seconds == 279u);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NONE);

    result = feed_quiet(&detector, candidate_time + 300u, 70);
    EXPECT(result.evidence_seconds == NAP_DETECTOR_ALARM_SECONDS);
    EXPECT(result.action == NAP_DETECTOR_ACTION_ALARM);

    result = feed_quiet(&detector, candidate_time + 420u, 70);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NONE);
}

static void test_reported_zero_vmc_full_drop_nudges_before_three_minutes(void) {
    NapDetector detector;
    NapDetectorResult result;
    NapDetectorMotion motion;
    uint32_t candidate_time;

    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 68u);
    (void)feed_quiet(&detector, 3000u, 68);
    (void)feed_quiet(&detector, 3120u, 68);
    (void)feed_quiet(&detector, 3240u, 68);

    motion = quiet_motion(3360u);
    motion.latest_vmc = 0u;
    (void)feed(&detector, 3360u, 57, motion);
    motion = quiet_motion(3480u);
    motion.latest_vmc = 0u;
    result = feed(&detector, 3480u, 57, motion);
    candidate_time = 3480u;
    EXPECT(result.positive);
    EXPECT(result.hr_full_drop);
    EXPECT(result.phase == NAP_DETECTOR_CANDIDATE);

    motion = quiet_motion(candidate_time + 120u);
    motion.latest_vmc = 0u;
    result = feed(&detector, candidate_time + 120u, 57, motion);
    EXPECT(result.evidence_seconds == 120u);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NUDGE);
    EXPECT(result.phase == NAP_DETECTOR_NUDGED);

    motion = quiet_motion(candidate_time + 180u);
    motion.latest_vmc = 0u;
    result = feed(&detector, candidate_time + 180u, 57, motion);
    EXPECT(result.evidence_seconds == 180u);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NONE);
}

static void test_movement_resets_candidate(void) {
    NapDetector detector;
    NapDetectorResult result;
    uint32_t candidate_time = prepare_balanced_candidate(&detector, 4000u);
    int32_t baseline_before = detector.baseline_q8;

    result = feed_quiet(&detector, candidate_time + 120u, 70);
    EXPECT(result.evidence_seconds == 120u);
    result = feed(&detector, candidate_time + 240u, 95,
                  movement_motion(candidate_time + 240u));

    EXPECT(result.sample_accepted);
    EXPECT(result.movement);
    EXPECT(result.phase == NAP_DETECTOR_ARMED);
    EXPECT(result.evidence_seconds == 0u);
    EXPECT(!result.request_fast_sampling);
    /* The movement that terminates a candidate must not update its baseline. */
    EXPECT(detector.baseline_q8 == baseline_before);
}

static void test_isolated_hr_outlier_is_median_filtered(void) {
    NapDetector detector;
    NapDetectorResult result;

    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 80u);
    (void)feed_quiet(&detector, 6000u, 80);
    (void)feed_quiet(&detector, 6120u, 80);
    (void)feed_quiet(&detector, 6240u, 80);

    result = feed_quiet(&detector, 6360u, 40);
    EXPECT(result.hr_valid);
    EXPECT(result.smoothed_hr_bpm == 80u);
    EXPECT(!result.hr_full_drop);
    EXPECT(!result.positive);
    EXPECT(result.phase == NAP_DETECTOR_ARMED);
}

static void test_soft_drop_plateau_sustains_candidate(void) {
    NapDetector detector;
    NapDetectorResult result;

    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 100u);
    (void)feed_quiet(&detector, 7000u, 100);
    (void)feed_quiet(&detector, 7120u, 100);
    (void)feed_quiet(&detector, 7240u, 100);
    (void)feed_quiet(&detector, 7360u, 94);
    result = feed_quiet(&detector, 7480u, 94);
    EXPECT(result.hr_soft_drop);
    EXPECT(!result.hr_full_drop);
    EXPECT(result.hr_falling);
    EXPECT(result.positive);
    EXPECT(result.phase == NAP_DETECTOR_CANDIDATE);

    result = feed_quiet(&detector, 7600u, 94);
    EXPECT(!result.hr_falling);
    EXPECT(result.hr_soft_drop);
    EXPECT(result.positive);
    EXPECT(result.evidence_seconds == 120u);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NUDGE);
}

static void test_soft_only_episode_nudges_but_never_alarms(void) {
    NapDetector detector;
    NapDetectorResult result;
    uint32_t candidate_time;

    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 100u);
    (void)feed_quiet(&detector, 7800u, 100);
    (void)feed_quiet(&detector, 7920u, 100);
    (void)feed_quiet(&detector, 8040u, 100);
    (void)feed_quiet(&detector, 8160u, 94);
    result = feed_quiet(&detector, 8280u, 94);
    candidate_time = 8280u;
    EXPECT(result.positive);
    EXPECT(result.hr_soft_drop);
    EXPECT(!result.hr_full_drop);

    result = feed_quiet(&detector, candidate_time + 99u, 94);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NONE);
    result = feed_quiet(&detector, candidate_time + 120u, 94);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NUDGE);
    result = feed_quiet(&detector, candidate_time + 279u, 94);
    EXPECT(result.phase == NAP_DETECTOR_NUDGED);
    result = feed_quiet(&detector, candidate_time + 300u, 94);

    EXPECT(result.hr_soft_drop);
    EXPECT(!result.hr_full_drop);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NONE);
    EXPECT(result.phase == NAP_DETECTOR_ARMED);
    EXPECT(result.evidence_seconds == 0u);
    EXPECT(!result.request_fast_sampling);
}

static void test_duplicate_and_clock_boundaries(void) {
    NapDetector detector;
    NapDetectorResult result;
    uint32_t candidate_time = prepare_balanced_candidate(&detector, 8000u);

    result = feed_quiet(&detector, candidate_time + 19u, 70);
    EXPECT(!result.sample_accepted);
    EXPECT(result.evidence_seconds == 0u);

    result = feed_quiet(&detector, candidate_time + 20u, 70);
    EXPECT(result.sample_accepted);
    EXPECT(result.positive);
    EXPECT(result.evidence_seconds == 20u);

    result = feed_quiet(&detector, candidate_time + 20u, 200);
    EXPECT(!result.sample_accepted);
    EXPECT(result.evidence_seconds == 20u);

    /* A clock rollback starts a fresh timeline instead of locking processing
     * until the previous high-water timestamp is reached. */
    result = feed_quiet(&detector, candidate_time, 70);
    EXPECT(result.sample_accepted);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NONE);
    EXPECT(result.phase == NAP_DETECTOR_ARMED);
    EXPECT(result.evidence_seconds == 0u);
    EXPECT(result.baseline_hr_bpm == 80u);

    result = feed_quiet(&detector, candidate_time + 20u, 70);
    EXPECT(result.sample_accepted);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NONE);
    EXPECT(result.phase == NAP_DETECTOR_ARMED);
}

static void test_missing_gap_cannot_bridge_evidence(void) {
    NapDetector detector;
    NapDetectorResult result;
    uint32_t candidate_time = prepare_balanced_candidate(&detector, 10000u);

    result = feed_quiet(&detector, candidate_time + 120u, 70);
    EXPECT(result.evidence_seconds == 120u);
    result = feed_quiet(&detector, candidate_time + 361u, 70);
    EXPECT(result.sample_accepted);
    EXPECT(result.phase == NAP_DETECTOR_ARMED);
    EXPECT(result.evidence_seconds == 0u);
    EXPECT(!result.positive);

    /* Exactly 240 seconds remains continuous; only a greater gap resets. */
    candidate_time = prepare_balanced_candidate(&detector, 12000u);
    result = feed_quiet(&detector, candidate_time + 240u, 70);
    EXPECT(result.action == NAP_DETECTOR_ACTION_NUDGE);
    EXPECT(result.evidence_seconds == 240u);
}

static void test_baseline_calibration_and_freezing(void) {
    NapDetector detector;
    NapDetectorResult result;
    uint32_t timestamp = 14000u;
    unsigned int index;
    int32_t baseline_before;
    int32_t expected_baseline;

    /* Exercise is neither a seed nor a steady-state baseline signal. */
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    for (index = 0u; index < 3u; index++) {
        result = feed(&detector, timestamp, 150, exercise_motion(timestamp));
        timestamp += 120u;
        EXPECT(result.baseline_hr_bpm == 0u);
    }

    /* Clear movement also cannot seed a resting baseline. */
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    for (index = 0u; index < 3u; index++) {
        result = feed(&detector, timestamp, 100, movement_motion(timestamp));
        timestamp += 120u;
        EXPECT(result.baseline_hr_bpm == 0u);
    }

    /* A stable quiet cluster establishes its robust median. */
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    (void)feed_quiet(&detector, timestamp, 72);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 73);
    timestamp += 120u;
    result = feed_quiet(&detector, timestamp, 72);
    EXPECT(result.baseline_hr_bpm == 72u);

    /* A rapid falling sequence is not a stable awake cluster. */
    timestamp += 120u;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    (void)feed_quiet(&detector, timestamp, 82);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 76);
    timestamp += 120u;
    result = feed_quiet(&detector, timestamp, 70);
    EXPECT(result.baseline_hr_bpm == 0u);
    EXPECT(detector.seed_count == 2u);

    /* A plausible HR outlier is discarded by retaining the closest pair. */
    timestamp += 120u;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    (void)feed_quiet(&detector, timestamp, 72);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 150);
    timestamp += 120u;
    result = feed_quiet(&detector, timestamp, 73);
    EXPECT(result.baseline_hr_bpm == 0u);
    EXPECT(detector.seed_count == 2u);
    timestamp += 120u;
    result = feed_quiet(&detector, timestamp, 74);
    EXPECT(result.baseline_hr_bpm == 73u);

    /* Motion invalidates a partial resting cluster. */
    timestamp += 120u;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    (void)feed_quiet(&detector, timestamp, 72);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 73);
    EXPECT(detector.seed_count == 2u);
    timestamp += 120u;
    (void)feed(&detector, timestamp, 100, movement_motion(timestamp));
    EXPECT(detector.seed_count == 0u);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 74);
    EXPECT(detector.seed_count == 1u);
    EXPECT(nap_detector_baseline_bpm(&detector) == 0u);

    /* Ambiguous motion or an invalid HR also breaks a stable seed run. */
    timestamp += 120u;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    (void)feed_quiet(&detector, timestamp, 72);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 73);
    timestamp += 120u;
    NapDetectorMotion incomplete_motion = quiet_motion(timestamp);
    incomplete_motion.valid_minutes = 3u;
    incomplete_motion.quiet_minutes = 3u;
    (void)feed(&detector, timestamp, 74, incomplete_motion);
    EXPECT(detector.seed_count == 0u);

    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 72);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 73);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 0);
    EXPECT(detector.seed_count == 0u);

    /* Stale motion and exercise each invalidate a partial quiet cluster. */
    timestamp += 120u;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    (void)feed_quiet(&detector, timestamp, 72);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 73);
    timestamp += 120u;
    result = feed(&detector, timestamp, 74,
                  quiet_motion(timestamp - 121u));
    EXPECT(!result.motion_fresh);
    EXPECT(detector.seed_count == 0u);

    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 72);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 73);
    timestamp += 120u;
    (void)feed(&detector, timestamp, 100, exercise_motion(timestamp));
    EXPECT(detector.seed_count == 0u);

    /* A long sample gap also starts calibration from scratch. */
    timestamp += 120u;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    (void)feed_quiet(&detector, timestamp, 72);
    (void)feed_quiet(&detector, timestamp + 120u, 73);
    result = feed_quiet(&detector, timestamp + 361u, 74);
    EXPECT(result.baseline_hr_bpm == 0u);
    EXPECT(detector.seed_count == 1u);

    /* Movement never updates an established resting baseline. */
    timestamp += 600u;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 80u);
    baseline_before = detector.baseline_q8;
    (void)feed(&detector, timestamp, 100, movement_motion(timestamp));
    timestamp += 120u;
    (void)feed(&detector, timestamp, 100, movement_motion(timestamp));
    timestamp += 120u;
    (void)feed(&detector, timestamp, 100, movement_motion(timestamp));
    EXPECT(detector.baseline_q8 == baseline_before);

    /* Quiet near-baseline HR adapts slowly with the 1/64 fixed-point EMA. */
    timestamp += 120u;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 80u);
    baseline_before = detector.baseline_q8;
    (void)feed_quiet(&detector, timestamp, 82);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 82);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 82);
    EXPECT(detector.baseline_q8 > baseline_before);
    EXPECT(detector.baseline_q8 < (int32_t)82 * 256);

    /* An upward spike is capped to a 10% target before applying the EMA. */
    timestamp += 120u;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 80u);
    baseline_before = detector.baseline_q8;
    (void)feed_quiet(&detector, timestamp, 100);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 100);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 100);
    expected_baseline = baseline_before +
        (((baseline_before + baseline_before / 10) - baseline_before) / 64);
    EXPECT(detector.baseline_q8 == expected_baseline);

    /* Soft-low and candidate samples freeze the established baseline. */
    timestamp += 120u;
    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 100u);
    (void)feed_quiet(&detector, timestamp, 100);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 100);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 100);
    baseline_before = detector.baseline_q8;
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 94);
    timestamp += 120u;
    result = feed_quiet(&detector, timestamp, 94);
    EXPECT(result.phase == NAP_DETECTOR_CANDIDATE);
    timestamp += 120u;
    (void)feed_quiet(&detector, timestamp, 94);
    EXPECT(detector.baseline_q8 == baseline_before);

    /* Explicit resets retain calibration while clearing episode state. */
    nap_detector_reset_transient(&detector);
    EXPECT(nap_detector_baseline_bpm(&detector) == 100u);
    EXPECT(detector.phase == NAP_DETECTOR_ARMED);
}

static bool target_hr_is_positive(NapDetectorSensitivity sensitivity,
                                  int16_t target_hr) {
    NapDetector detector;
    NapDetectorResult result;

    nap_detector_init(&detector, sensitivity);
    nap_detector_restore_baseline(&detector, 100u);
    (void)feed_quiet(&detector, 17000u, 100);
    (void)feed_quiet(&detector, 17120u, 100);
    (void)feed_quiet(&detector, 17240u, 100);
    (void)feed_quiet(&detector, 17360u, target_hr);
    result = feed_quiet(&detector, 17480u, target_hr);
    return result.positive;
}

static void test_sensitivity_is_monotonic(void) {
    bool sensitive;
    bool balanced;
    bool conservative;

    sensitive = target_hr_is_positive(NAP_DETECTOR_SENSITIVE, 95);
    balanced = target_hr_is_positive(NAP_DETECTOR_BALANCED, 95);
    conservative = target_hr_is_positive(NAP_DETECTOR_CONSERVATIVE, 95);
    EXPECT(sensitive);
    EXPECT(!balanced);
    EXPECT(!conservative);

    sensitive = target_hr_is_positive(NAP_DETECTOR_SENSITIVE, 93);
    balanced = target_hr_is_positive(NAP_DETECTOR_BALANCED, 93);
    conservative = target_hr_is_positive(NAP_DETECTOR_CONSERVATIVE, 93);
    EXPECT(sensitive);
    EXPECT(balanced);
    EXPECT(!conservative);

    sensitive = target_hr_is_positive(NAP_DETECTOR_SENSITIVE, 90);
    balanced = target_hr_is_positive(NAP_DETECTOR_BALANCED, 90);
    conservative = target_hr_is_positive(NAP_DETECTOR_CONSERVATIVE, 90);
    EXPECT(sensitive);
    EXPECT(balanced);
    EXPECT(conservative);
}

static void test_motion_freshness_and_hr_gates(void) {
    NapDetector detector;
    NapDetectorMotion motion;
    NapDetectorResult result;

    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 80u);

    motion = quiet_motion(19880u);
    result = feed(&detector, 20000u, 80, motion);
    EXPECT(result.motion_fresh);
    EXPECT(result.quiet);

    motion = quiet_motion(19919u);
    result = feed(&detector, 20040u, 80, motion);
    EXPECT(!result.motion_fresh);
    EXPECT(!result.quiet);

    result = feed_quiet(&detector, 20160u, 34);
    EXPECT(result.sample_accepted);
    EXPECT(!result.hr_valid);
    result = feed_quiet(&detector, 20280u, 221);
    EXPECT(!result.hr_valid);
}

static void test_four_valid_quiet_minutes_are_sufficient(void) {
    NapDetector detector;
    NapDetectorMotion motion;
    NapDetectorResult result;

    nap_detector_init(&detector, NAP_DETECTOR_BALANCED);
    nap_detector_restore_baseline(&detector, 80u);

    motion = quiet_motion(21000u);
    motion.valid_minutes = 4u;
    motion.quiet_minutes = 4u;
    result = feed(&detector, 21000u, 80, motion);
    EXPECT(result.motion_fresh);
    EXPECT(result.quiet);

    motion = quiet_motion(21120u);
    motion.valid_minutes = 3u;
    motion.quiet_minutes = 3u;
    result = feed(&detector, 21120u, 80, motion);
    EXPECT(result.motion_fresh);
    EXPECT(!result.quiet);
}

static void test_ambiguous_samples_do_not_bridge(void) {
    NapDetector detector;
    NapDetectorResult result;
    uint32_t candidate_time = prepare_balanced_candidate(&detector, 22000u);

    result = feed_quiet(&detector, candidate_time + 120u, 70);
    EXPECT(result.evidence_seconds == 120u);

    /* Missing HR pauses the timer and breaks consecutiveness. */
    result = feed_quiet(&detector, candidate_time + 240u, 0);
    EXPECT(!result.hr_valid);
    EXPECT(result.evidence_seconds == 120u);
    result = feed_quiet(&detector, candidate_time + 360u, 70);
    EXPECT(result.positive);
    EXPECT(result.evidence_seconds == 120u);

    /* A valid quiet negative decays evidence rather than hard-resetting it. */
    (void)feed_quiet(&detector, candidate_time + 480u, 80);
    result = feed_quiet(&detector, candidate_time + 600u, 80);
    EXPECT(!result.positive);
    EXPECT(result.evidence_seconds < 240u);
    EXPECT(result.evidence_seconds > 0u);

    /* Frequent missing callbacks cannot keep an old candidate alive forever. */
    candidate_time = prepare_balanced_candidate(&detector, 24000u);
    result = feed_quiet(&detector, candidate_time + 120u, 0);
    EXPECT(result.phase == NAP_DETECTOR_CANDIDATE);
    result = feed_quiet(&detector, candidate_time + 240u, 0);
    EXPECT(result.phase == NAP_DETECTOR_CANDIDATE);
    result = feed_quiet(&detector, candidate_time + 260u, 0);
    EXPECT(result.phase == NAP_DETECTOR_ARMED);
    EXPECT(result.evidence_seconds == 0u);
}

int main(void) {
    test_quiet_awake_has_no_action();
    test_real_doze_nudges_and_alarms_at_exact_boundaries();
    test_reported_zero_vmc_full_drop_nudges_before_three_minutes();
    test_movement_resets_candidate();
    test_isolated_hr_outlier_is_median_filtered();
    test_soft_drop_plateau_sustains_candidate();
    test_soft_only_episode_nudges_but_never_alarms();
    test_duplicate_and_clock_boundaries();
    test_missing_gap_cannot_bridge_evidence();
    test_baseline_calibration_and_freezing();
    test_sensitivity_is_monotonic();
    test_motion_freshness_and_hr_gates();
    test_four_valid_quiet_minutes_are_sufficient();
    test_ambiguous_samples_do_not_bridge();

    if (failures != 0u) {
        (void)fprintf(stderr, "%u detector assertion(s) failed\n", failures);
        return 1;
    }

    (void)puts("nap_detector: all deterministic tests passed");
    return 0;
}
