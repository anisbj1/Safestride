// F13 wiring seams (additive to §9's files): compiles wire_baseline.cpp into
// the host suite (README §3: the wiring pair must build for native too — F17's
// pattern), pins the §8 pump constants, and drives the construction / load /
// publish paths without FreeRTOS.
//
// §9 tests 15 and 18 live here; the DoD §14 present-side test (F06 assembler
// with a live baseline) is NOT in F13 Owns — the integrator adds it to
// test/test_wave6_dataflow at P4 (spec §13).

#include "../../../src/app/wiring/wire_baseline.cpp"

#include <unity.h>

#include <bw/baseline/baseline_config.h>
#include <bw/baseline/baseline_serializer.h>
#include <bw/testkit/fake_clock.h>
#include <bw/testkit/mem_kv_store.h>
#include <bw/testkit/recording_fault_reporter.h>

#include <cstring>

#include "fixtures.h"

namespace bl = bw::baseline;

namespace {
// Private fault-injection wrapper: keep the shared F00 fake as the byte store,
// and observe attempted writes (including refused ones) at the pump boundary.
class CountingStore final : public bw::IKeyValueStore {
public:
    bw::Result<size_t> get(const char* key, void* buf, size_t cap) override {
        return bytes.get(key, buf, cap);
    }
    bw::Result<void> put(const char* key, const void* data, size_t len) override {
        ++attempts;
        return fail_writes ? bw::Result<void>{bw::Err::BusError} : bytes.put(key, data, len);
    }
    bw::testkit::InMemoryKeyValueStore<> bytes;
    uint32_t attempts = 0;
    bool fail_writes = false;
};

void publishEvidence(app::AppContext& ctx, bw::TimeMs t) {
    bw::MotionFeatures motion{};
    motion.t_ms = t;
    motion.quality = bw::DataQuality::Good;
    motion.enmo_g = 0.05f;
    bw::PpgFeatures ppg{};
    ppg.t_ms = t;
    ppg.quality = bw::DataQuality::Good;
    ppg.hr_valid = true;
    ppg.hr_bpm = 60.0f;
    bw::ActivityEstimate activity{};
    activity.t_ms = t;
    activity.context = bw::ActivityContext::Stationary;
    bw::WearEstimate wear{};
    wear.t_ms = t;
    wear.state = bw::WearState::Worn;
    ctx.motion_features.set(motion);
    ctx.ppg_features.set(ppg);
    ctx.activity_estimate.set(activity);
    ctx.wear_estimate.set(wear);
}
}  // namespace

void test_pump_persist_cadence_and_failed_write_retry(void) {
    app::AppContext ctx{};
    bw::testkit::FakeClock clock;
    CountingStore store;
    ctx.clock = &clock;
    f13::wireWithStore(ctx, &store);
    auto tick = [&](bw::TimeMs now) {
        clock.setMs(now);
        publishEvidence(ctx, now);
        f13::pumpOnce(ctx);
    };

    tick(bl::kPersistIntervalMs - f13::kBaselinePumpPeriodMs);
    TEST_ASSERT_EQUAL_UINT32(0, store.attempts);
    store.fail_writes = true;
    tick(bl::kPersistIntervalMs);
    TEST_ASSERT_EQUAL_UINT32(1, store.attempts);
    TEST_ASSERT_EQUAL_UINT32(2, f13::tracker().state().accepted_samples);
    bw::BaselineProfile profile{};
    TEST_ASSERT_TRUE(f13::provider().profile(profile));  // failure never stops publication
    TEST_ASSERT_FALSE(profile.valid);

    // A dying store must not turn a background learner into a 1 Hz flash writer.
    store.fail_writes = false;
    tick(bl::kPersistIntervalMs + f13::kBaselinePumpPeriodMs);
    tick(2 * bl::kPersistIntervalMs - f13::kBaselinePumpPeriodMs);
    TEST_ASSERT_EQUAL_UINT32(1, store.attempts);
    tick(2 * bl::kPersistIntervalMs);
    TEST_ASSERT_EQUAL_UINT32(2, store.attempts);
    bl::TrackerState saved{};
    TEST_ASSERT_TRUE(bl::BaselineSerializer::loadFromStore(&store, saved).ok());
    TEST_ASSERT_TRUE(f13_test::sameState(saved, f13::tracker().state()));
    TEST_ASSERT_EQUAL_UINT32(5, saved.accepted_samples);  // gaps aren't fabricated evidence

    // Reboot through the same wiring load path, not just the pure serializer.
    f13::wireWithStore(ctx, &store);
    TEST_ASSERT_TRUE(f13::provider().profile(profile));
    TEST_ASSERT_FALSE(profile.valid);
    TEST_ASSERT_TRUE(f13_test::sameState(saved, f13::tracker().state()));
}

void test_pump_persists_valid_transition_before_next_interval(void) {
    app::AppContext ctx{};
    bw::testkit::FakeClock clock;
    CountingStore store;
    ctx.clock = &clock;
    // Restore one sample short of valid. The next evidence window arrives well
    // before the interval, so only the false->true trigger can save it.
    const bl::TrackerState seeded{60.0f, 4.0f, 0.05f, bl::kMinQualifiedMs / bl::kMsPerSecond - 1};
    TEST_ASSERT_TRUE(bl::BaselineSerializer::saveToStore(&store, seeded).ok());
    store.attempts = 0;
    f13::wireWithStore(ctx, &store);
    bw::BaselineProfile profile{};
    TEST_ASSERT_TRUE(f13::provider().profile(profile));
    TEST_ASSERT_FALSE(profile.valid);
    clock.setMs(f13::kBaselinePumpPeriodMs);
    publishEvidence(ctx, clock.nowMs());
    f13::pumpOnce(ctx);
    TEST_ASSERT_EQUAL_UINT32(1, store.attempts);
    TEST_ASSERT_TRUE(f13::provider().profile(profile));
    TEST_ASSERT_TRUE(profile.valid);
    bl::TrackerState saved{};
    TEST_ASSERT_TRUE(bl::BaselineSerializer::loadFromStore(&store, saved).ok());
    TEST_ASSERT_TRUE(f13_test::sameState(saved, f13::tracker().state()));

    clock.setMs(2 * f13::kBaselinePumpPeriodMs);
    publishEvidence(ctx, clock.nowMs());
    f13::pumpOnce(ctx);
    TEST_ASSERT_EQUAL_UINT32(1, store.attempts);  // valid isn't a per-tick save trigger
    f13::wireWithStore(ctx, &store);
    TEST_ASSERT_TRUE(f13::provider().profile(profile));
    TEST_ASSERT_TRUE(profile.valid);  // immediately usable after reboot, with no evidence
    TEST_ASSERT_TRUE(f13_test::sameState(saved, f13::tracker().state()));
}

void test_pump_counts_only_new_sensor_pairs(void) {
    app::AppContext ctx{};
    bw::testkit::FakeClock clock;
    ctx.clock = &clock;
    f13::wireWithStore(ctx, nullptr);
    clock.setMs(1000);
    publishEvidence(ctx, 1000);
    f13::pumpOnce(ctx);
    TEST_ASSERT_EQUAL_UINT32(1, f13::tracker().state().accepted_samples);
    clock.setMs(2000);
    f13::pumpOnce(ctx);  // Held mailboxes remain fresh, but aren't new evidence.
    TEST_ASSERT_EQUAL_UINT32(1, f13::tracker().state().accepted_samples);
    publishEvidence(ctx, 2000);
    f13::pumpOnce(ctx);
    TEST_ASSERT_EQUAL_UINT32(2, f13::tracker().state().accepted_samples);
    bw::BaselineProfile p{};
    TEST_ASSERT_TRUE(f13::provider().profile(p));
    TEST_ASSERT_FALSE(p.valid);
}

void test_pump_rejects_stale_context(void) {
    app::AppContext ctx{};
    bw::testkit::FakeClock clock;
    ctx.clock = &clock;
    f13::wireWithStore(ctx, nullptr);
    const bw::TimeMs now = bl::kMaxSampleAgeMs + 1000;
    clock.setMs(now);
    publishEvidence(ctx, now);
    bw::WearEstimate old_wear{};
    old_wear.state = bw::WearState::Worn;
    ctx.wear_estimate.set(old_wear);
    f13::pumpOnce(ctx);
    TEST_ASSERT_EQUAL_UINT32(0, f13::tracker().state().accepted_samples);
    publishEvidence(ctx, now);
    bw::ActivityEstimate old_activity{};
    old_activity.context = bw::ActivityContext::Stationary;
    ctx.activity_estimate.set(old_activity);
    f13::pumpOnce(ctx);
    TEST_ASSERT_EQUAL_UINT32(0, f13::tracker().state().accepted_samples);
    publishEvidence(ctx, now);
    bw::StateSnapshot old_snapshot{};
    old_snapshot.quality = {bw::DataQuality::Good, bw::DataQuality::Good};
    ctx.state_snapshot.set(old_snapshot);
    f13::pumpOnce(ctx);
    TEST_ASSERT_EQUAL_UINT32(0, f13::tracker().state().accepted_samples);
    // New evidence and current context allow learning again.
    clock.setMs(now + f13::kBaselinePumpPeriodMs);
    publishEvidence(ctx, clock.nowMs());
    old_snapshot.t_ms = clock.nowMs();
    ctx.state_snapshot.set(old_snapshot);
    f13::pumpOnce(ctx);
    TEST_ASSERT_EQUAL_UINT32(1, f13::tracker().state().accepted_samples);
}

void test_pump_hold_without_evidence_arms_cooldown(void) {
    app::AppContext ctx{};
    bw::testkit::FakeClock clock;
    ctx.clock = &clock;
    f13::wireWithStore(ctx, nullptr);
    bw::AlertStatus alert{};
    alert.level = bw::AlertLevel::Active;
    ctx.alert_status.set(alert);
    f13::pumpOnce(ctx);  // No sensor mailboxes: the hold must still be observed.
    alert.level = bw::AlertLevel::Normal;
    ctx.alert_status.set(alert);
    for (uint32_t i = 0; i < bl::kPostEventHoldSamples; ++i) {
        clock.setMs((static_cast<bw::TimeMs>(i) + 1) * f13::kBaselinePumpPeriodMs);
        publishEvidence(ctx, clock.nowMs());
        f13::pumpOnce(ctx);
        TEST_ASSERT_EQUAL_UINT32(0, f13::tracker().state().accepted_samples);
    }
    clock.setMs((static_cast<bw::TimeMs>(bl::kPostEventHoldSamples) + 1) *
                f13::kBaselinePumpPeriodMs);
    publishEvidence(ctx, clock.nowMs());
    f13::pumpOnce(ctx);
    TEST_ASSERT_EQUAL_UINT32(1, f13::tracker().state().accepted_samples);
}

// §9.15 — the publish seam F06 consumes: absent before the first publish,
// present (with the valid flag intact) after it, and wire()'s re-construction
// must not leave a stale answer behind.
void test_adapter_publish_semantics(void) {
    f13::BaselineProviderAdapter adapter;
    bw::BaselineProfile out{};
    out.valid = true;                         // poison: untouched until a real publish
    TEST_ASSERT_FALSE(adapter.profile(out));  // the F06 "absent" case

    bw::BaselineProfile p{};
    p.valid = true;
    p.resting_hr_bpm = 61.5f;
    p.resting_hr_mad = 4.0f;
    p.typical_enmo_g = 0.05f;
    p.confidence_pct = 25;
    p.version = bl::kProfileVersion;
    adapter.publish(p);

    TEST_ASSERT_TRUE(adapter.profile(out));
    TEST_ASSERT_TRUE(out.valid);
    TEST_ASSERT_EQUAL_FLOAT(p.resting_hr_bpm, out.resting_hr_bpm);
    TEST_ASSERT_EQUAL_FLOAT(p.resting_hr_mad, out.resting_hr_mad);
    TEST_ASSERT_EQUAL_FLOAT(p.typical_enmo_g, out.typical_enmo_g);
    TEST_ASSERT_EQUAL_UINT8(p.confidence_pct, out.confidence_pct);
    TEST_ASSERT_EQUAL_UINT16(p.version, out.version);

    adapter.reset();
    TEST_ASSERT_FALSE(adapter.profile(out));
}

// §9.18 — no IKeyValueStore: the composition root boots in memory, the tracker
// starts empty, learns and runs (§10 failure behavior).
void test_store_absent_boots_in_memory(void) {
    app::AppContext ctx{};
    f13::wire(ctx);  // host build: the no-store path, by construction

    TEST_ASSERT_NOT_NULL(&f13::tracker());
    bw::BaselineProfile p{};
    TEST_ASSERT_FALSE(f13::provider().profile(p));  // nothing learned, nothing fabricated

    const bl::StepResult r = f13::tracker().step(f13_test::goodSample());
    TEST_ASSERT_TRUE(r.accepted);
    bw::BaselineProfile learned{};
    TEST_ASSERT_TRUE(f13::tracker().profile(learned));  // the TRACKER learned and runs
    TEST_ASSERT_FALSE(learned.valid);                   // honest: below kMinQualifiedMs
    TEST_ASSERT_EQUAL_UINT32(1, f13::tracker().state().accepted_samples);
    // D4: the provider is the ADAPTER — it changes only when something
    // publishes. On the host there is no pump, so it must still read as
    // absent; on the device the pump publishes within one 1 Hz tick.
    TEST_ASSERT_FALSE(f13::provider().profile(p));
}

// Additive wiring pin (F17's test_wiring_constants_match_spec pattern): the §8
// values the integrator creates the P4 task from.
void test_wiring_constants_match_spec(void) {
    TEST_ASSERT_EQUAL_UINT32(1000, f13::kBaselinePumpPeriodMs);    // §8 [DERIVED] 1 Hz grid
    TEST_ASSERT_EQUAL_UINT32(2048, f13::kBaselinePumpStackBytes);  // §8 [EXPERIMENTAL]
    TEST_ASSERT_EQUAL_UINT32(2, f13::kBaselinePumpPriority);       // §8 [DERIVED] below F06
    TEST_ASSERT_EQUAL_UINT32(0, f13::kBaselinePumpCore);           // §8 [DERIVED] D7 core 0
}

// Additive D6/§10 check (the CR landed at W6 G1): an unreadable blob under
// kStoreKey is discarded WITH FaultCode::BaselineCorrupt = 25 at Warning, and
// an empty store (first boot) or a valid blob report nothing.
void test_wire_corrupt_store_reports_baseline_fault(void) {
    bw::testkit::InMemoryKeyValueStore<> store;
    uint8_t garbage[bl::kBlobBytes];
    std::memset(garbage, 0xDE, sizeof(garbage));
    TEST_ASSERT_TRUE(store.put(bl::kStoreKey, garbage, sizeof(garbage)).ok());

    app::AppContext ctx{};
    bw::testkit::RecordingFaultReporter faults;
    ctx.faults = &faults;  // ctx.clock stays null: the report degrades to t = 0
    f13::wireWithStore(ctx, &store);

    TEST_ASSERT_EQUAL_UINT32(1, faults.count());
    TEST_ASSERT_EQUAL(static_cast<int>(bw::FaultCode::BaselineCorrupt),
                      static_cast<int>(faults.at(0).code));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::FaultSeverity::Warning),
                      static_cast<int>(faults.at(0).severity));
    bw::BaselineProfile p{};
    TEST_ASSERT_FALSE(f13::provider().profile(p));  // discarded ⇒ learning restarts empty

    // First boot: a missing key is NOT a fault (§10).
    bw::testkit::InMemoryKeyValueStore<> empty;
    app::AppContext ctx2{};
    bw::testkit::RecordingFaultReporter quiet;
    ctx2.faults = &quiet;
    f13::wireWithStore(ctx2, &empty);
    TEST_ASSERT_EQUAL_UINT32(0, quiet.count());
    TEST_ASSERT_FALSE(f13::provider().profile(p));
}

// Additive §10 restore path: a valid blob is readable through the provider
// BEFORE the first pump tick, with no fault reported.
void test_wire_restores_valid_blob(void) {
    const bl::TrackerState seeded{61.0f, 4.0f, 0.05f, 7200};
    uint8_t blob[64];
    TEST_ASSERT_TRUE(bl::BaselineSerializer::serialize(seeded, blob, sizeof(blob)).ok());

    bw::testkit::InMemoryKeyValueStore<> store;
    TEST_ASSERT_TRUE(store.put(bl::kStoreKey, blob, bl::kBlobBytes).ok());

    app::AppContext ctx{};
    bw::testkit::RecordingFaultReporter faults;
    ctx.faults = &faults;
    f13::wireWithStore(ctx, &store);

    TEST_ASSERT_EQUAL_UINT32(0, faults.count());
    bw::BaselineProfile p{};
    TEST_ASSERT_TRUE(f13::provider().profile(p));  // restored, no tick needed
    TEST_ASSERT_TRUE(p.valid);
    TEST_ASSERT_EQUAL_FLOAT(61.0f, p.resting_hr_bpm);
    TEST_ASSERT_EQUAL_UINT32(7200, f13::tracker().state().accepted_samples);
}
