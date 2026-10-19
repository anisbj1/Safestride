#include <bw/alert/alert_engine.h>
#include <bw/alert/alert_policy.h>
#include <initializer_list>
#include <unity.h>

namespace {
using namespace bw;
using namespace bw::alert;

AlertConfig demoConfig() {
    AlertConfig cfg;
    cfg.demo_wake_protocol = true;
    return cfg;
}

struct Demo {
    AlertPolicy policy{demoConfig()};
    PolicyInput in{};
    Demo() {
        in.has_state = true;
        in.state.wear = WearState::Worn;
        in.state.alertness = AlertnessState::Awake;
        in.state.quality = {DataQuality::Good, DataQuality::Good};
    }
    PolicyOutput tick(TimeMs t, ReasonMask reasons = 0, bool ack = false, bool cancel = false,
                      bool gentle_motion = false, TimeMs motion_stamp = 0) {
        in.t_ms = t;
        in.state.t_ms = t;
        in.state.reasons = reasons;
        in.state.wake_motion = {motion_stamp == 0 ? t : motion_stamp, true,
                                gentle_motion ||
                                    (reasons == reasonBit(ReasonCode::ShortMotionBurst))};
        in.acknowledge = ack;
        in.cancel = cancel;
        return policy.evaluate(in);
    }
};

void level(AlertLevel expected, const PolicyOutput& out) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(out.status.level));
}

void test_demo_motion_restarts_interval_and_quiet_awake_does_not() {
    Demo d;
    level(AlertLevel::Normal, d.tick(0));
    const ReasonMask motion = reasonBit(ReasonCode::ShortMotionBurst);
    level(AlertLevel::Normal, d.tick(kDemoCheckIntervalMs - 1, motion));
    level(AlertLevel::Normal, d.tick(kDemoCheckIntervalMs));
    const TimeMs next = kDemoCheckIntervalMs * 2 - 1;
    level(AlertLevel::Warning, d.tick(next));
    const auto answered = d.tick(next + kDemoResponseWindowMs - 1, 0, true);
    level(AlertLevel::Normal, answered);
    TEST_ASSERT_TRUE(answered.wake_check_passed);
    const TimeMs answer = answered.status.t_ms;
    level(AlertLevel::Normal, d.tick(answer + kDemoCheckIntervalMs - 1));
    level(AlertLevel::Warning, d.tick(answer + kDemoCheckIntervalMs));
}

void test_demo_deadline_short_ignored_red_latches_until_long() {
    Demo d;
    (void)d.tick(0);
    (void)d.tick(kDemoCheckIntervalMs);
    auto out = d.tick(kDemoCheckIntervalMs + kDemoResponseWindowMs, 0, true);
    level(AlertLevel::Active, out);
    TEST_ASSERT_FALSE(out.wake_check_passed);
    out = d.tick(kDemoCheckIntervalMs + kDemoResponseWindowMs + 1,
                 reasonBit(ReasonCode::ShortMotionBurst), true);
    level(AlertLevel::Active, out);
    TEST_ASSERT_FALSE(out.wake_check_passed);
    // No Persistent auto-stop/cooldown silently clears a latched demo alert.
    level(AlertLevel::Active, d.tick(kPersistentMaxMs + kDemoCheckIntervalMs));
    const TimeMs cancel = kPersistentMaxMs + kDemoCheckIntervalMs + 1;
    out = d.tick(cancel, 0, false, true);
    level(AlertLevel::Normal, out);
    TEST_ASSERT_TRUE(out.wake_check_passed);
    level(AlertLevel::Warning, d.tick(cancel + kDemoCheckIntervalMs));
}

void test_demo_motion_clears_orange_but_not_at_deadline() {
    Demo d;
    (void)d.tick(0);
    (void)d.tick(kDemoCheckIntervalMs);
    const ReasonMask motion = reasonBit(ReasonCode::ShortMotionBurst);
    auto out = d.tick(kDemoCheckIntervalMs + 1, motion);
    level(AlertLevel::Normal, out);
    TEST_ASSERT_TRUE(out.wake_check_passed);
    const TimeMs second = kDemoCheckIntervalMs * 2 + 1;
    (void)d.tick(second);
    out = d.tick(second + kDemoResponseWindowMs, motion);
    level(AlertLevel::Active, out);
    TEST_ASSERT_FALSE(out.wake_check_passed);
}

void test_demo_burst_eight_seconds_still_orange_first() {
    Demo d;
    (void)d.tick(0);
    const ReasonMask low = reasonBit(ReasonCode::SustainedLowMotion);
    (void)d.tick(1000, reasonBit(ReasonCode::ShortMotionBurst));
    (void)d.tick(2000, low | reasonBit(ReasonCode::ShortMotionBurst));
    for (TimeMs t = 3000; t < 2000 + kDemoBurstStillnessMs; t += 1000) {
        level(AlertLevel::Normal, d.tick(t, low));
    }
    const TimeMs check = 2000 + kDemoBurstStillnessMs;
    level(AlertLevel::Warning, d.tick(check, low));
    level(AlertLevel::Warning, d.tick(check + kDemoResponseWindowMs - 1, low));
    level(AlertLevel::Active, d.tick(check + kDemoResponseWindowMs, low));
}

void test_demo_motion_and_evidence_gap_break_stillness() {
    for (bool gap : {false, true}) {
        Demo d;
        (void)d.tick(0);
        const ReasonMask low = reasonBit(ReasonCode::SustainedLowMotion);
        (void)d.tick(1000, low | reasonBit(ReasonCode::ShortMotionBurst));
        (void)d.tick(2000, low);
        (void)d.tick(gap ? 4000 : 3000, gap ? low : 0);
        for (TimeMs t = 5000; t <= 12000; t += 1000) {
            level(AlertLevel::Normal, d.tick(t, low));
        }
    }
}

void test_demo_stale_notworn_unknown_and_imu_loss_abort_without_response() {
    for (unsigned gate = 0; gate < 5; ++gate) {
        Demo d;
        (void)d.tick(0);
        (void)d.tick(kDemoCheckIntervalMs);
        (void)d.tick(kDemoCheckIntervalMs + kDemoResponseWindowMs);
        d.in.acknowledge = true;
        if (gate == 0) {
            d.in.has_state = false;
            d.in.t_ms += kStateStaleMs + 1;
        } else {
            ++d.in.t_ms;
            d.in.state.t_ms = d.in.t_ms;
            if (gate == 1)
                d.in.state.wear = WearState::NotWorn;
            if (gate == 2)
                d.in.state.alertness = AlertnessState::Unknown;
            if (gate == 3)
                d.in.state.quality.imu = DataQuality::Insufficient;
            if (gate == 4)
                d.in.state.wear = WearState::Uncertain;
        }
        const auto stopped = d.policy.evaluate(d.in);
        level(AlertLevel::Normal, stopped);
        TEST_ASSERT_FALSE(stopped.wake_check_passed);
        d.in.has_state = true;
        d.in.state.wear = WearState::Worn;
        d.in.state.alertness = AlertnessState::Awake;
        d.in.state.quality.imu = DataQuality::Good;
        const TimeMs rewear = stopped.status.t_ms + 1;
        level(AlertLevel::Normal, d.tick(rewear));
        level(AlertLevel::Normal, d.tick(rewear + kDemoCheckIntervalMs - 1));
        level(AlertLevel::Warning, d.tick(rewear + kDemoCheckIntervalMs));
    }
}

void test_demo_long_at_deadline_wins_and_simultaneous_ack_does_not_override() {
    Demo d;
    (void)d.tick(0);
    (void)d.tick(kDemoCheckIntervalMs);
    const auto out = d.tick(kDemoCheckIntervalMs + kDemoResponseWindowMs, 0, true, true);
    level(AlertLevel::Normal, out);
    TEST_ASSERT_TRUE(out.wake_check_passed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AlertPhase::Cancelled),
                            out.event.payload[kAlertPayloadPhaseIndex]);
}

void test_demo_64bit_time_and_late_tick_no_catchup() {
    Demo d;
    const TimeMs boot = TimeMs{1} << 33;
    (void)d.tick(boot);
    const TimeMs late = boot + kDemoCheckIntervalMs * 10;
    level(AlertLevel::Warning, d.tick(late));
    level(AlertLevel::Warning, d.tick(late + kDemoResponseWindowMs - 1));
    level(AlertLevel::Active, d.tick(late + kDemoResponseWindowMs));
}

void test_demo_mode_ceiling_caps_red_and_restores_latch() {
    auto cfg = demoConfig();
    cfg.mode_intensity[static_cast<unsigned>(OperatingMode::Night)].max_level = AlertLevel::Warning;
    Demo d;
    d.policy = AlertPolicy{cfg};
    d.in.state.mode = OperatingMode::Night;
    (void)d.tick(0);
    (void)d.tick(kDemoCheckIntervalMs);
    level(AlertLevel::Warning, d.tick(kDemoCheckIntervalMs + kDemoResponseWindowMs));
    d.in.state.mode = OperatingMode::Day;
    level(AlertLevel::Active, d.tick(kDemoCheckIntervalMs + kDemoResponseWindowMs + 1));
}

void test_demo_engine_forwards_only_accepted_response_for_composition() {
    AlertEngine engine{demoConfig()};
    Demo feed;
    feed.in.t_ms = 0;
    (void)engine.step(feed.in);
    feed.in.t_ms = kDemoCheckIntervalMs;
    feed.in.state.t_ms = feed.in.t_ms;
    auto out = engine.step(feed.in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertLevel::Warning),
                          static_cast<int>(out.status.level));
    TEST_ASSERT_FALSE(out.wake_check_passed);
    ++feed.in.t_ms;
    feed.in.state.t_ms = feed.in.t_ms;
    feed.in.acknowledge = true;
    out = engine.step(feed.in);
    TEST_ASSERT_TRUE(out.wake_check_passed);
    TEST_ASSERT_TRUE(out.level_changed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertLevel::Normal), static_cast<int>(out.status.level));
}

void test_demo_queued_release_before_deadline_is_accepted_but_old_or_future_is_not() {
    for (unsigned candidate = 0; candidate < 4; ++candidate) {
        Demo d;
        (void)d.tick(0);
        (void)d.tick(kDemoCheckIntervalMs);
        d.in.acknowledge_has_timestamp = true;
        const TimeMs deadline = kDemoCheckIntervalMs + kDemoResponseWindowMs;
        d.in.acknowledge_t_ms = candidate == 0   ? deadline - 1
                                : candidate == 1 ? deadline
                                : candidate == 2 ? kDemoCheckIntervalMs - 1
                                                 : deadline + 1;
        const auto out = d.tick(deadline, 0, true);
        level(candidate == 0 ? AlertLevel::Normal : AlertLevel::Active, out);
        TEST_ASSERT_EQUAL(candidate == 0, out.wake_check_passed);
    }
}
void test_demo_gentle_motion_deduplicated_fresh_and_red_immune() {
    Demo d;
    (void)d.tick(1000);
    auto out = d.tick(2000, 0, false, false, true);
    TEST_ASSERT_EQUAL_UINT64(2000 + kDemoCheckIntervalMs, out.demo.next_check_ms);
    out = d.tick(2500, 0, false, false, true, 2000);
    TEST_ASSERT_EQUAL_UINT64(2000 + kDemoCheckIntervalMs, out.demo.next_check_ms);
    // Future and stale qualifying evidence cannot defer the check.
    (void)d.tick(3000, 0, false, false, true, 4000);
    (void)d.tick(3000 + kDemoMotionFreshnessMs + 1, 0, false, false, true, 3000);
    out = d.tick(2000 + kDemoCheckIntervalMs);
    level(AlertLevel::Warning, out);
    TEST_ASSERT_EQUAL_UINT64(out.status.t_ms + kDemoResponseWindowMs,
                             out.demo.response_deadline_ms);
    out = d.tick(out.status.t_ms + 1000, 0, false, false, true);
    level(AlertLevel::Normal, out);
    TEST_ASSERT_TRUE(out.wake_check_passed);
    const TimeMs next = out.demo.next_check_ms;
    (void)d.tick(next);
    (void)d.tick(next + kDemoResponseWindowMs);
    out = d.tick(next + kDemoResponseWindowMs + 1, 0, false, false, true);
    level(AlertLevel::Active, out);
    TEST_ASSERT_TRUE(out.demo.red_latched);
    TEST_ASSERT_FALSE(out.wake_check_passed);
    TEST_ASSERT_EQUAL_UINT64(0, out.demo.next_check_ms);
}

void test_demo_phased_motion_restarts_every_window_not_every_pipeline_delay() {
    // [DERIVED] F02 publish + F06 consumption + F07 forwarding + F10 tick.
    // Each stage is independently phased; the ORIGINAL window-end is retained.
    constexpr TimeMs kPublicationLagMs = 1000;
    constexpr TimeMs kWakePhaseMs = 992;  // bench measured F06 age = 1992 ms
    constexpr TimeMs kStatePhaseMs = 900;
    constexpr TimeMs kWindowMs = 1000;
    constexpr TimeMs kBootOffsetMs = TimeMs{1} << 32;
    const TimeMs pipeline_age_ms =
        kPublicationLagMs + kWakePhaseMs + kStatePhaseMs + kAlertTickPeriodMs;
    Demo d;
    (void)d.tick(kBootOffsetMs);
    PolicyOutput out{};
    for (TimeMs window = 1; window <= 40; ++window) {
        const TimeMs source_ms = kBootOffsetMs + window * kWindowMs;
        const TimeMs now = source_ms + pipeline_age_ms;
        out = d.tick(now, 0, false, false, true, source_ms);
        level(AlertLevel::Normal, out);
        // Even when source_ms predates the PREVIOUS reset's consumption,
        // this new observation must restart a full twenty seconds.
        TEST_ASSERT_EQUAL_UINT64(now + kDemoCheckIntervalMs, out.demo.next_check_ms);
        // Re-forwarding that same observation on a later snapshot is not motion.
        out = d.tick(now + kAlertTickPeriodMs, 0, false, false, true, source_ms);
        TEST_ASSERT_EQUAL_UINT64(now + kDemoCheckIntervalMs, out.demo.next_check_ms);
    }
    const TimeMs deadline = out.demo.next_check_ms;
    level(AlertLevel::Normal, d.tick(deadline - 1));
    level(AlertLevel::Warning, d.tick(deadline));
    // A new movement DURING orange traverses the same pipeline and clears it.
    const TimeMs response_source_ms = deadline + kWindowMs;
    out = d.tick(response_source_ms + pipeline_age_ms, 0, false, false, true, response_source_ms);
    level(AlertLevel::Normal, out);
    TEST_ASSERT_TRUE(out.wake_check_passed);
    TEST_ASSERT_EQUAL_UINT64(response_source_ms + pipeline_age_ms + kDemoCheckIntervalMs,
                             out.demo.next_check_ms);
}

void test_demo_motion_age_boundary_is_not_the_stillness_gap() {
    // [DERIVED] Exact end-to-end age accepted; one millisecond older refused.
    for (const TimeMs age : {TimeMs{kDemoMotionFreshnessMs}, TimeMs{kDemoMotionFreshnessMs + 1}}) {
        Demo d;
        (void)d.tick(1000);
        const TimeMs source_ms = 2000;
        const TimeMs now = source_ms + age;
        auto out = d.tick(now, 0, false, false, true, source_ms);
        const bool accepted = age == kDemoMotionFreshnessMs;
        TEST_ASSERT_EQUAL_UINT64((accepted ? now : 1000) + kDemoCheckIntervalMs,
                                 out.demo.next_check_ms);
    }
    // The older snapshot continuity bound is intentionally unchanged.
    TEST_ASSERT_EQUAL_UINT32(1500, kDemoEvidenceGapMs);
}

void test_demo_capped_red_cannot_be_cleared_by_short_or_motion() {
    Demo d;
    auto cfg = demoConfig();
    cfg.mode_intensity[static_cast<unsigned>(OperatingMode::Night)].max_level = AlertLevel::Warning;
    d.policy = AlertPolicy{cfg};
    d.in.state.mode = OperatingMode::Night;
    (void)d.tick(0);
    (void)d.tick(kDemoCheckIntervalMs);
    (void)d.tick(kDemoCheckIntervalMs + kDemoResponseWindowMs);
    auto out = d.tick(kDemoCheckIntervalMs + kDemoResponseWindowMs + 1, 0, true, false, true);
    TEST_ASSERT_TRUE(out.demo.red_latched);
    TEST_ASSERT_FALSE(out.wake_check_passed);
    d.in.state.mode = OperatingMode::Day;
    level(AlertLevel::Active, d.tick(kDemoCheckIntervalMs + kDemoResponseWindowMs + 2));
}

void test_demo_invalid_motion_and_pre_gate_evidence_cannot_restart() {
    Demo d;
    (void)d.tick(1000);
    d.in.t_ms = 2000;
    d.in.state.t_ms = 2000;
    d.in.state.wake_motion = {2000, false, true};
    auto out = d.policy.evaluate(d.in);
    TEST_ASSERT_EQUAL_UINT64(1000 + kDemoCheckIntervalMs, out.demo.next_check_ms);
    d.in.state.wear = WearState::NotWorn;
    out = d.tick(3000, 0, false, false, true);
    TEST_ASSERT_FALSE(out.demo.monitoring);
    TEST_ASSERT_FALSE(out.wake_check_passed);
    d.in.state.wear = WearState::Worn;
    out = d.tick(4000, 0, false, false, true, 3000);
    TEST_ASSERT_EQUAL_UINT64(4000 + kDemoCheckIntervalMs, out.demo.next_check_ms);
    out = d.tick(4500, 0, false, false, true, 3000);
    TEST_ASSERT_EQUAL_UINT64(4000 + kDemoCheckIntervalMs, out.demo.next_check_ms);
    level(AlertLevel::Warning, d.tick(4000 + kDemoCheckIntervalMs));
}
}  // namespace

void register_demo_protocol_tests() {
    RUN_TEST(test_demo_phased_motion_restarts_every_window_not_every_pipeline_delay);
    RUN_TEST(test_demo_motion_age_boundary_is_not_the_stillness_gap);
    RUN_TEST(test_demo_gentle_motion_deduplicated_fresh_and_red_immune);
    RUN_TEST(test_demo_capped_red_cannot_be_cleared_by_short_or_motion);
    RUN_TEST(test_demo_invalid_motion_and_pre_gate_evidence_cannot_restart);
    RUN_TEST(test_demo_motion_restarts_interval_and_quiet_awake_does_not);
    RUN_TEST(test_demo_deadline_short_ignored_red_latches_until_long);
    RUN_TEST(test_demo_motion_clears_orange_but_not_at_deadline);
    RUN_TEST(test_demo_burst_eight_seconds_still_orange_first);
    RUN_TEST(test_demo_motion_and_evidence_gap_break_stillness);
    RUN_TEST(test_demo_stale_notworn_unknown_and_imu_loss_abort_without_response);
    RUN_TEST(test_demo_long_at_deadline_wins_and_simultaneous_ack_does_not_override);
    RUN_TEST(test_demo_64bit_time_and_late_tick_no_catchup);
    RUN_TEST(test_demo_mode_ceiling_caps_red_and_restores_latch);
    RUN_TEST(test_demo_engine_forwards_only_accepted_response_for_composition);
    RUN_TEST(test_demo_queued_release_before_deadline_is_accepted_but_old_or_future_is_not);
}
