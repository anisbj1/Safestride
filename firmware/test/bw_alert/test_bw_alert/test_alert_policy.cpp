// bw_alert - AlertPolicy tests (F10 §9): the §6 rule table row by row, the
// ladder timeline, acknowledgment / cancel / cooldown / snooze, the stale-input
// failure behaviour of §10 and the event floor of §6 D4. Every scenario is a
// timeline of (t_ms, feed) pairs - no wall clock, no sleeps.

#include <unity.h>

#include <bw/alert/alert_policy.h>

#include "alert_fixtures.h"

using f10test::assertLevel;
using f10test::assertNoChange;
using f10test::assertPhase;
using f10test::Feed;
using f10test::hasReason;
using f10test::step;
namespace ba = bw::alert;
using bw::AlertLevel;
using bw::TimeMs;

namespace {

// The ladder thresholds the timeline is written against (F10 §8).
constexpr TimeMs kWarnMs = ba::kWarningToActiveMs;
constexpr TimeMs kHoldMs = ba::kDeescalateHoldMs;
constexpr TimeMs kStaleMs = ba::kStateStaleMs;

Feed drowsy() {
    Feed f;
    f.alertness = bw::AlertnessState::Drowsy;
    return f;
}

Feed risk() {
    Feed f;
    f.alertness = bw::AlertnessState::HighDrowsinessRisk;
    return f;
}

Feed sleeping() {
    Feed f;
    f.alertness = bw::AlertnessState::PossibleSleep;
    return f;
}

Feed noSnapshot() {
    Feed f;
    f.has_snapshot = false;
    return f;
}

}  // namespace

// --- startup / empty state (host-testing item 2) ----------------------------

void test_policy_boot_has_no_alert(void) {
    ba::AlertPolicy policy;
    const ba::PolicyOutput out = step(policy, 0, noSnapshot());

    assertNoChange(out, "first call with no state must not change the level");
    assertLevel(AlertLevel::Normal, out, "no data at boot => Normal");
    TEST_ASSERT_TRUE_MESSAGE(hasReason(out.status.reasons, bw::ReasonCode::SensorUnavailable),
                             "no data must be explained as SensorUnavailable");
    TEST_ASSERT_FALSE(out.status.acknowledged);
    TEST_ASSERT_EQUAL_UINT32(0, out.status.since_ms);
    TEST_ASSERT_EQUAL_UINT64(0, out.status.t_ms);
}

// --- §6 rule table (mirrors the spec row for row) ---------------------------

namespace {

struct RuleRow {
    const char* name;
    bw::WearState wear;
    bw::AlertnessState alertness;
    AlertLevel expected;
};

}  // namespace

void test_policy_rules_table(void) {
    // Row ids mirror F10 §6.1 exactly: R1 no/stale snapshot, R2 NotWorn,
    // R3 Unknown, R4 Awake, R5 Drowsy, R6 HighDrowsinessRisk, R7 PossibleSleep
    // (the trailing "-" row of the table is the Uncertain-wear note).
    const RuleRow rows[] = {
        {"R4 worn + Awake => Normal", bw::WearState::Worn, bw::AlertnessState::Awake,
         AlertLevel::Normal},
        {"R5 worn + Drowsy => Warning", bw::WearState::Worn, bw::AlertnessState::Drowsy,
         AlertLevel::Warning},
        {"R6 worn + HighDrowsinessRisk => Active", bw::WearState::Worn,
         bw::AlertnessState::HighDrowsinessRisk, AlertLevel::Active},
        {"R7 worn + PossibleSleep => Persistent", bw::WearState::Worn,
         bw::AlertnessState::PossibleSleep, AlertLevel::Persistent},
        {"R3 alertness Unknown => Normal", bw::WearState::Worn, bw::AlertnessState::Unknown,
         AlertLevel::Normal},
        {"R2 alertness NotWorn => Normal", bw::WearState::Worn, bw::AlertnessState::NotWorn,
         AlertLevel::Normal},
        {"R2 wear NotWorn + Awake => Normal", bw::WearState::NotWorn, bw::AlertnessState::Awake,
         AlertLevel::Normal},
        {"R2 wear NotWorn + Drowsy => Normal (gate wins)", bw::WearState::NotWorn,
         bw::AlertnessState::Drowsy, AlertLevel::Normal},
        {"- wear Uncertain + Drowsy => Warning (conservative note row)", bw::WearState::Uncertain,
         bw::AlertnessState::Drowsy, AlertLevel::Warning},
    };

    for (const RuleRow& row : rows) {
        ba::AlertPolicy policy;
        Feed f;
        f.wear = row.wear;
        f.alertness = row.alertness;
        const ba::PolicyOutput out = step(policy, 1000, f);
        assertLevel(row.expected, out, row.name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(row.expected != AlertLevel::Normal, out.level_changed ? 1 : 0,
                                      row.name);
    }
}

void test_policy_notworn_carries_device_not_worn_reason(void) {
    ba::AlertPolicy policy;
    Feed f;
    f.wear = bw::WearState::NotWorn;
    const ba::PolicyOutput out = step(policy, 1000, f);

    assertLevel(AlertLevel::Normal, out, "not worn never alerts");
    TEST_ASSERT_TRUE_MESSAGE(hasReason(out.status.reasons, bw::ReasonCode::DeviceNotWorn),
                             "the gate reason must be visible to F22/F12");
}

// --- ladder: immediate up, held down (§6 D1) --------------------------------

void test_policy_raises_warning_with_explainable_event(void) {
    ba::AlertPolicy policy;
    const ba::PolicyOutput out = step(policy, 1000, drowsy());

    assertLevel(AlertLevel::Warning, out, "Drowsy raises Warning on the same tick");
    assertPhase(ba::AlertPhase::Raised, out, "a raise is reported as AlertRaised/Raised");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertLevel::Warning),
                          static_cast<int>(f10test::levelOfEvent(out.event)));
    TEST_ASSERT_EQUAL_UINT64(1000, out.event.t_start_ms);
    TEST_ASSERT_EQUAL_UINT32(0, out.event.duration_ms);
    TEST_ASSERT_EQUAL_FLOAT(0.75f, out.event.confidence);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::EventType::AlertRaised),
                          static_cast<int>(out.event.type));
    TEST_ASSERT_EQUAL_UINT8(ba::kAlertFeatureId, out.event.detector_id);
    TEST_ASSERT_EQUAL_UINT8(bw::Event::kSchemaVersion, out.event.schema_version);
    TEST_ASSERT_EQUAL_UINT32(0, out.event.seq);  // F17 assigns seq (02 §4.5)
    TEST_ASSERT_EQUAL_UINT32(0, out.event.boot_id);
    TEST_ASSERT_TRUE_MESSAGE(hasReason(out.event.reasons, bw::ReasonCode::SustainedLowMotion),
                             "the event carries the state's own reasons");
    TEST_ASSERT_TRUE_MESSAGE(hasReason(out.status.reasons, bw::ReasonCode::SustainedLowMotion),
                             "AlertStatus explains the level it is in");
    TEST_ASSERT_EQUAL_UINT32(0, out.status.since_ms);
}

void test_policy_warning_escalates_after_persistence(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, drowsy());

    const ba::PolicyOutput early = step(policy, 1000 + kWarnMs - 1, drowsy());
    assertLevel(AlertLevel::Warning, early,
                "1 ms before kWarningToActiveMs the ladder is still silent");

    const ba::PolicyOutput at = step(policy, 1000 + kWarnMs, drowsy());
    assertLevel(AlertLevel::Active, at, "at kWarningToActiveMs it beeps");
    assertPhase(ba::AlertPhase::Raised, at, "the escalation is itself an AlertRaised");

    const ba::PolicyOutput held = step(policy, 1000 + kWarnMs + 5000, drowsy());
    assertLevel(AlertLevel::Active, held, "Active persists while the condition does");
    assertNoChange(held, "an unchanged level emits no event");
}

// --- §6.1 stage 6: the sustained-demand timer (demand VALUE semantics) -----

void test_policy_active_to_warning_stepdown_is_reachable(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, risk());     // level Active, demand Active
    const TimeMs low_since = 2000;  // demand drops to Warning here
    step(policy, low_since, drowsy());

    const ba::PolicyOutput early = step(policy, low_since + kHoldMs - 1, drowsy());
    assertLevel(AlertLevel::Active, early, "the step-down still waits out kDeescalateHoldMs");

    const ba::PolicyOutput at = step(policy, low_since + kHoldMs, drowsy());
    assertLevel(AlertLevel::Warning, at,
                "Active => Warning is reachable: the sustained timer does not block the hold");
    assertNoChange(step(policy, low_since + kHoldMs + 1000, drowsy()),
                   "the Warning level then holds while the demand stays Warning");
}

void test_policy_warning_level_sustained_escalates_back(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, risk());
    const TimeMs demand_warning_since = 2000;
    step(policy, demand_warning_since, drowsy());
    const TimeMs stepped_down = demand_warning_since + kHoldMs;
    step(policy, stepped_down, drowsy());  // level Warning since here

    const ba::PolicyOutput early = step(policy, demand_warning_since + kWarnMs - 1, drowsy());
    assertLevel(AlertLevel::Warning, early,
                "Warning demand short of kWarningToActiveMs keeps the Warning level");

    const ba::PolicyOutput at = step(policy, demand_warning_since + kWarnMs, drowsy());
    assertLevel(AlertLevel::Active, at,
                "Warning demand sustained >= kWarningToActiveMs while at Warning escalates back");
    assertPhase(ba::AlertPhase::Raised, at, "the re-escalation is one AlertRaised event");
}

void test_policy_demand_change_restarts_the_sustained_timer(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, risk());  // demand Active - must NOT feed the Warning timer
    const TimeMs demand_warning_since = 2000;
    step(policy, demand_warning_since, drowsy());            // demand changes to Warning here
    step(policy, demand_warning_since + kHoldMs, drowsy());  // level Warning

    // kWarningToActiveMs after the ACTIVE demand: the old timer start would
    // escalate right here, re-raising 5 s after the step-down.
    const ba::PolicyOutput no_early = step(policy, 1000 + kWarnMs, drowsy());
    assertLevel(AlertLevel::Warning, no_early,
                "a demand change restarts the timer: no early re-escalation");

    const ba::PolicyOutput at = step(policy, demand_warning_since + kWarnMs, drowsy());
    assertLevel(AlertLevel::Active, at,
                "the escalation lands kWarningToActiveMs after the demand became Warning");
}

void test_policy_deescalation_between_levels_reports_raised(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, risk());    // Active
    step(policy, 2000, drowsy());  // demand Warning: hold starts

    const ba::PolicyOutput down = step(policy, 2000 + kHoldMs, drowsy());
    assertLevel(AlertLevel::Warning, down, "the drop between two non-Normal levels completes");
    TEST_ASSERT_TRUE_MESSAGE(down.level_changed, "one transition, therefore one event");
    assertPhase(ba::AlertPhase::Raised, down,
                "§6.2: Resolved only on the way to Normal, Raised between levels");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertLevel::Warning),
                          static_cast<int>(f10test::levelOfEvent(down.event)));
}

void test_policy_deescalation_holds_the_old_level(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, risk());
    const TimeMs low_since = 2000;  // first Awake evaluation
    step(policy, low_since, Feed{});

    const ba::PolicyOutput early = step(policy, low_since + kHoldMs - 1, Feed{});
    assertLevel(AlertLevel::Active, early,
                "the level is held for kDeescalateHoldMs after the condition clears");

    const ba::PolicyOutput at = step(policy, low_since + kHoldMs, Feed{});
    assertLevel(AlertLevel::Normal, at, "de-escalation completes exactly at the hold");
    assertPhase(ba::AlertPhase::Resolved, at, "a self-resolved alert is Resolved");
    TEST_ASSERT_TRUE_MESSAGE(hasReason(at.event.reasons, bw::ReasonCode::SustainedLowMotion),
                             "the resolved record keeps the reasons the alert was raised with");
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(at.event.duration_ms),
                             static_cast<uint32_t>(kHoldMs + 1000));
}

// --- suppression windows (§6 D5, §8) ----------------------------------------

void test_policy_resolution_starts_cooldown(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, drowsy());
    step(policy, 2000, Feed{});  // Awake: hold starts
    const TimeMs resolved_at = 2000 + kHoldMs;
    step(policy, resolved_at, Feed{});  // Normal + cooldown

    const ba::PolicyOutput blocked = step(policy, resolved_at + 1000, drowsy());
    assertLevel(AlertLevel::Normal, blocked, "cooldown suppresses an immediate re-raise");

    const ba::PolicyOutput after = step(policy, resolved_at + ba::kCooldownMs, drowsy());
    assertLevel(AlertLevel::Warning, after, "after kCooldownMs the same condition alerts");
}

void test_policy_ack_stops_and_snoozes_same_level(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, drowsy());

    Feed ack = drowsy();
    ack.acknowledge = true;
    const ba::PolicyOutput acked = step(policy, 2000, ack);
    assertLevel(AlertLevel::Normal, acked, "acknowledge silences immediately");
    assertPhase(ba::AlertPhase::Acknowledged, acked, "the stop is logged as Acknowledged");
    TEST_ASSERT_TRUE(acked.status.acknowledged);

    const ba::PolicyOutput inside = step(policy, 2000 + ba::kAckSnoozeMs - 1, drowsy());
    assertLevel(AlertLevel::Normal, inside, "same-level demand is snoozed");
    TEST_ASSERT_TRUE_MESSAGE(inside.status.acknowledged,
                             "acknowledged stays visible during the snooze");

    const ba::PolicyOutput at = step(policy, 2000 + ba::kAckSnoozeMs, drowsy());
    assertLevel(AlertLevel::Warning, at, "the snooze ends exactly at kAckSnoozeMs");
    TEST_ASSERT_FALSE_MESSAGE(at.status.acknowledged, "a new alert starts unacknowledged");
}

void test_policy_ack_never_blocks_a_higher_level(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, drowsy());

    Feed ack = drowsy();
    ack.acknowledge = true;
    step(policy, 2000, ack);  // snoozed at Warning

    const ba::PolicyOutput escalation = step(policy, 3000, risk());
    assertLevel(AlertLevel::Active, escalation,
                "an ack suppresses the acked level, never a higher one");
    TEST_ASSERT_FALSE(escalation.status.acknowledged);
}

void test_policy_ack_with_no_alert_is_a_noop(void) {
    ba::AlertPolicy policy;
    Feed f;
    f.acknowledge = true;  // Awake: nothing to acknowledge
    const ba::PolicyOutput out = step(policy, 1000, f);

    assertNoChange(out, "acking silence changes nothing");
    TEST_ASSERT_FALSE(out.status.acknowledged);
}

void test_policy_cancel_starts_cooldown(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, drowsy());

    Feed cancel = drowsy();
    cancel.cancel = true;
    const ba::PolicyOutput stopped = step(policy, 2000, cancel);
    assertLevel(AlertLevel::Normal, stopped, "cancel silences immediately");
    assertPhase(ba::AlertPhase::Cancelled, stopped, "the stop is logged as Cancelled");

    const ba::PolicyOutput blocked = step(policy, 3000, drowsy());
    assertLevel(AlertLevel::Normal, blocked, "cancel suppresses re-raising for kCooldownMs");

    const ba::PolicyOutput after = step(policy, 2000 + ba::kCooldownMs, drowsy());
    assertLevel(AlertLevel::Warning, after, "after the cooldown the ladder runs again");
}

void test_policy_cancel_held_across_ticks_pushes_cooldown_each_tick(void) {
    // §6.1 one-tick input contract: commands are pulses, and the policy does
    // NOT edge-detect them - a command level wrongly held across ticks
    // re-applies every tick. Three consecutive cancel ticks therefore push
    // kCooldownMs forward by three ticks (the documented cost of the contract;
    // see F10 §6.1 and the status log for the choice made).
    ba::AlertPolicy policy;
    step(policy, 1000, drowsy());

    Feed cancel = drowsy();
    cancel.cancel = true;
    const TimeMs first_tick = 2000;
    step(policy, first_tick, cancel);       // tick 1: cooldown until first_tick + kCooldownMs
    step(policy, first_tick + 50, cancel);  // tick 2: pushed
    const TimeMs last_tick = first_tick + 100;
    step(policy, last_tick, cancel);  // tick 3: pushed again

    const ba::PolicyOutput pushed_past_first = step(policy, first_tick + ba::kCooldownMs, drowsy());
    assertLevel(AlertLevel::Normal, pushed_past_first,
                "the cooldown outlives its first tick: the hold pushed it forward");

    const ba::PolicyOutput blocked = step(policy, last_tick + ba::kCooldownMs - 1, drowsy());
    assertLevel(AlertLevel::Normal, blocked, "still inside the pushed cooldown");

    const ba::PolicyOutput after = step(policy, last_tick + ba::kCooldownMs, drowsy());
    assertLevel(AlertLevel::Warning, after,
                "the cooldown ends exactly kCooldownMs after the LAST cancel tick");
}

// --- §6 D3: Persistent gives up ---------------------------------------------

void test_policy_persistent_auto_stops_at_max_duration(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, sleeping());

    const TimeMs start = 1000;
    const ba::PolicyOutput early = step(policy, start + ba::kPersistentMaxMs - 1, sleeping());
    assertLevel(AlertLevel::Persistent, early, "Persistent runs until kPersistentMaxMs");

    const ba::PolicyOutput stop = step(policy, start + ba::kPersistentMaxMs, sleeping());
    assertLevel(AlertLevel::Normal, stop, "then it stops by itself");
    assertPhase(ba::AlertPhase::AutoStoppedMaxDuration, stop,
                "and logs that it stopped for that reason");
    TEST_ASSERT_EQUAL_UINT32(ba::kPersistentMaxMs, stop.event.duration_ms);

    const ba::PolicyOutput blocked = step(policy, start + ba::kPersistentMaxMs + 1, sleeping());
    assertLevel(AlertLevel::Normal, blocked, "the give-up starts a cooldown, no loop");
}

void test_policy_persistent_clock_restarts_on_reentry(void) {
    // §6.2 last line: "Persistent clock restarts on entering Persistent" -
    // a NEW Persistent entry gets a fresh kPersistentMaxMs timer, it never
    // inherits the timestamp of an earlier one.
    ba::AlertPolicy policy;
    constexpr TimeMs kFirstStart = 1000;
    step(policy, kFirstStart, sleeping());  // Persistent #1, clock starts here

    // Stop it through the stale path (§10: no cooldown, so the ladder may
    // raise again immediately once a fresh snapshot arrives).
    const ba::PolicyOutput stale = step(policy, kFirstStart + kStaleMs + 1, noSnapshot());
    assertLevel(AlertLevel::Normal, stale, "the first Persistent entry ends via StateStale");

    constexpr TimeMs kSecondStart = 20000;
    const ba::PolicyOutput again = step(policy, kSecondStart, sleeping());
    assertLevel(AlertLevel::Persistent, again, "a fresh snapshot raises Persistent again");

    // Well past kFirstStart + kPersistentMaxMs (which an inherited clock would
    // already have hit), but still short of kSecondStart + kPersistentMaxMs.
    const ba::PolicyOutput still_running =
        step(policy, kSecondStart + ba::kPersistentMaxMs - 1, sleeping());
    assertLevel(AlertLevel::Persistent, still_running,
                "the re-entered Persistent runs its own fresh kPersistentMaxMs");

    const ba::PolicyOutput stop = step(policy, kSecondStart + ba::kPersistentMaxMs, sleeping());
    assertLevel(AlertLevel::Normal, stop, "and gives up one full kPersistentMaxMs later");
    assertPhase(ba::AlertPhase::AutoStoppedMaxDuration, stop, "with the same give-up phase");
}

// --- §10 failure behaviour --------------------------------------------------

void test_policy_stale_holds_then_deescalates(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, drowsy());

    // The producer goes quiet: the held snapshot ages.
    const ba::PolicyOutput holding = step(policy, 1000 + kStaleMs, noSnapshot());
    assertLevel(AlertLevel::Warning, holding, "the level is held for kStateStaleMs");
    assertNoChange(holding, "still within the hold: no transition");

    const ba::PolicyOutput stale = step(policy, 1000 + kStaleMs + 1, noSnapshot());
    assertLevel(AlertLevel::Normal, stale, "past kStateStaleMs the alert de-escalates");
    assertPhase(ba::AlertPhase::StateStale, stale, "and says why");
    TEST_ASSERT_TRUE_MESSAGE(hasReason(stale.status.reasons, bw::ReasonCode::SensorUnavailable),
                             "AlertStatus.reasons is SensorUnavailable while stale");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(stale.event.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(stale.event.quality.ppg));
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, stale.event.confidence,
                                    "no confidence may be fabricated from a dead input");
}

void test_policy_stale_recovery_starts_no_cooldown(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, drowsy());
    step(policy, 1000 + kStaleMs + 1, noSnapshot());

    const ba::PolicyOutput back = step(policy, 1000 + kStaleMs + 1000, drowsy());
    assertLevel(AlertLevel::Warning, back,
                "a fresh snapshot may alert immediately after a stale stop");
}

void test_policy_stale_at_32bit_boundary_time(void) {
    // Time beyond 32 bits must not truncate (01 §4, host-testing item 5).
    constexpr TimeMs kBeyond32BitMs = 5000000000ULL;
    ba::AlertPolicy policy;
    step(policy, kBeyond32BitMs, drowsy());

    const ba::PolicyOutput holding = step(policy, kBeyond32BitMs + kStaleMs, noSnapshot());
    assertLevel(AlertLevel::Warning, holding, "64-bit elapsed time holds normally");

    const ba::PolicyOutput stale = step(policy, kBeyond32BitMs + kStaleMs + 1, noSnapshot());
    assertLevel(AlertLevel::Normal, stale, "and detects staleness without wrapping");
}

void test_policy_32bit_duration_fields_saturate(void) {
    // 02 §4.5/§4.6: duration_ms and since_ms are uint32 - past 0xFFFFFFFF the
    // saturatingMs32 path must clamp, never wrap (F10 §8 kMaxDurationFieldMs).
    // Active demand is used on purpose: a Warning demand this old would
    // (correctly) escalate through §6.1 stage 6 and reset the level clock.
    constexpr TimeMs kBeyond32BitMs = 5000000000ULL;  // > 0xFFFFFFFF
    ba::AlertPolicy policy;
    step(policy, 1000, risk());

    const ba::PolicyOutput holding = step(policy, kBeyond32BitMs, risk());
    assertLevel(AlertLevel::Active, holding, "a 49-day-old level is still held");
    assertNoChange(holding, "and the condition itself did not change");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ba::kMaxDurationFieldMs, holding.status.since_ms,
                                     "AlertStatus.since_ms saturates at 0xFFFFFFFF");

    const ba::PolicyOutput low = step(policy, kBeyond32BitMs + 1, Feed{});  // hold starts
    assertNoChange(low, "the de-escalation hold runs on 64-bit time");

    const ba::PolicyOutput resolved = step(policy, kBeyond32BitMs + 1 + kHoldMs, Feed{});
    assertLevel(AlertLevel::Normal, resolved, "and the level resolves normally");
    assertPhase(ba::AlertPhase::Resolved, resolved, "as a Resolved record");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        ba::kMaxDurationFieldMs, resolved.event.duration_ms,
        "Event.duration_ms saturates at 0xFFFFFFFF instead of wrapping");
}

// --- §6 D4 event floor ------------------------------------------------------

namespace {

Feed withNotification(bw::EventType type) {
    Feed f;  // Awake: the state alone would never alert
    f.notification.present = true;
    f.notification.type = type;
    return f;
}

}  // namespace

void test_policy_event_floor_raises_and_expires(void) {
    ba::AlertPolicy policy;
    const TimeMs start = 1000;

    const ba::PolicyOutput raised =
        step(policy, start, withNotification(bw::EventType::SuspectedCataplexy));
    assertLevel(AlertLevel::Active, raised, "a suspected event floors the demand at Active");
    assertPhase(ba::AlertPhase::Raised, raised, "and raises an alert for it");

    const ba::PolicyOutput held = step(policy, start + ba::kEventAlertHoldMs, Feed{});
    assertLevel(AlertLevel::Active, held, "the floor holds for exactly kEventAlertHoldMs");

    const TimeMs expired_at = start + ba::kEventAlertHoldMs + 1;
    const ba::PolicyOutput expired = step(policy, expired_at, Feed{});
    assertLevel(AlertLevel::Active, expired, "afterwards the de-escalation hold starts");

    const ba::PolicyOutput let_go = step(policy, expired_at + kHoldMs, Feed{});
    assertLevel(AlertLevel::Normal, let_go, "the floor lets go after its hold");
    assertPhase(ba::AlertPhase::Resolved, let_go, "and the alert is Resolved");
}

void test_policy_event_floor_is_gated(void) {
    ba::AlertPolicy policy;
    Feed f = withNotification(bw::EventType::SuspectedCataplexy);
    f.wear = bw::WearState::NotWorn;

    const ba::PolicyOutput out = step(policy, 1000, f);
    assertLevel(AlertLevel::Normal, out, "no alert while NotWorn, event or not");
    assertNoChange(out, "the gate suppresses the floor entirely");
}

void test_policy_event_floor_held_while_gated_fires_after_reopen(void) {
    // §6 D4: a notification is held even while it is gated; it may still fire
    // if the gate reopens inside kEventAlertHoldMs (the hold clock runs from
    // the notification, not from the gate opening).
    const TimeMs start = 1000;
    Feed off_wrist = withNotification(bw::EventType::SuspectedCataplexy);
    off_wrist.wear = bw::WearState::NotWorn;

    ba::AlertPolicy policy;
    const ba::PolicyOutput gated = step(policy, start, off_wrist);
    assertLevel(AlertLevel::Normal, gated, "the notification arrives while the gate is closed");
    assertNoChange(gated, "and raises nothing on that tick");

    const ba::PolicyOutput reopened = step(policy, start + 15000, Feed{});
    assertLevel(AlertLevel::Active, reopened,
                "a held notification fires once the gate reopens inside the hold window");
    assertPhase(ba::AlertPhase::Raised, reopened, "as one AlertRaised of the floored level");

    // A gate that reopens only after the hold expired fires nothing.
    ba::AlertPolicy late;
    step(late, start, off_wrist);
    const ba::PolicyOutput expired = step(late, start + ba::kEventAlertHoldMs + 1, Feed{});
    assertLevel(AlertLevel::Normal, expired, "after kEventAlertHoldMs the held floor is gone");
    assertNoChange(expired, "and no alert is raised for it");
}

void test_policy_other_event_types_do_not_alert(void) {
    ba::AlertPolicy policy;
    const ba::PolicyOutput out = step(policy, 1000, withNotification(bw::EventType::Boot));
    assertLevel(AlertLevel::Normal, out, "only the two named types floor the demand");
}

void test_policy_drowsiness_event_floors_warning(void) {
    ba::AlertPolicy policy;
    const ba::PolicyOutput out =
        step(policy, 1000, withNotification(bw::EventType::DrowsinessEvent));
    assertLevel(AlertLevel::Warning, out, "a drowsiness event floors the demand at Warning");
}

// --- §6 D7 mode profile -----------------------------------------------------

void test_policy_mode_ceiling_caps_the_ladder(void) {
    ba::AlertConfig config;
    config.mode_intensity[static_cast<size_t>(static_cast<uint8_t>(bw::OperatingMode::Night))]
        .max_level = AlertLevel::Warning;
    ba::AlertPolicy policy(config);

    Feed f = risk();
    f.mode = bw::OperatingMode::Night;
    const ba::PolicyOutput out = step(policy, 1000, f);

    assertLevel(AlertLevel::Warning, out, "the mode ceiling caps the level");
    Feed day = risk();
    ba::AlertPolicy day_policy(config);
    const ba::PolicyOutput day_out = step(day_policy, 1000, day);
    assertLevel(AlertLevel::Active, day_out, "the same demand is uncapped in Day mode");
}

void test_policy_unknown_mode_falls_back_to_day(void) {
    ba::AlertConfig config;
    // An append-only enum value nobody tabled must never index off the end
    // (02 §6.6): the profile falls back to Day, which is the permissive one.
    const ba::ModeIntensity profile = config.modeIntensity(static_cast<bw::OperatingMode>(200));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertLevel::Persistent),
                          static_cast<int>(profile.max_level));
    TEST_ASSERT_EQUAL_UINT8(100, profile.led_gain_pct);
    TEST_ASSERT_TRUE(profile.buzzer_enabled);
}

// --- status bookkeeping (02 §4.6) -------------------------------------------

void test_policy_status_tracks_since_and_acknowledged(void) {
    ba::AlertPolicy policy;
    step(policy, 1000, drowsy());

    const ba::PolicyOutput later = step(policy, 6000, drowsy());
    assertNoChange(later, "a steady condition emits no event");
    TEST_ASSERT_EQUAL_UINT32(5000, later.status.since_ms);
    TEST_ASSERT_EQUAL_UINT64(6000, later.status.t_ms);
    TEST_ASSERT_FALSE(later.status.acknowledged);

    Feed ack = drowsy();
    ack.acknowledge = true;
    const ba::PolicyOutput acked = step(policy, 7000, ack);
    TEST_ASSERT_TRUE(acked.status.acknowledged);
    TEST_ASSERT_EQUAL_UINT32(0, acked.status.since_ms);
}

// --- budget (F10 §11: RAM < 1 KB for the whole feature) ---------------------

void test_policy_size_within_budget(void) {
    TEST_ASSERT_LESS_THAN_MESSAGE(512, sizeof(ba::AlertPolicy),
                                  "AlertPolicy must fit in half the 1 KB budget");
    TEST_ASSERT_LESS_THAN_MESSAGE(256, sizeof(ba::AlertConfig),
                                  "the config travels by value through queues");
}
