// F08 §9 test_recovery.cpp — §6 D4 / §10: the IKeyValueStore open marker.
// Marker present/absent/corrupt/schema-mismatch, recover() idempotence,
// kv == nullptr (the exact W2 build), marker clearing, and the put-count
// budget (1 open + 1 close per Active episode; candidates cost no writes).

#include <bw/sleep/sleep_episode_tracker.h>
#include <bw/testkit/mem_kv_store.h>
#include <unity.h>

#include "sleep_test_support.h"

using bw::EpisodeClass;
using bw::TimeMs;
using bw::sleep::OpenMarkerRecord;
using bw::sleep::SleepEpisodeConfig;
using bw::sleep::SleepEpisodeTracker;
using bw::sleep::StepOutput;
using f08_test::awake;
using f08_test::CountingKvStore;
using f08_test::makeSnapshot;

namespace {

constexpr SleepEpisodeConfig kCfg{};
constexpr TimeMs kT0 = 1000;
constexpr bw::UnixSeconds kSyncedUnix = 4242;

using MemKv = bw::testkit::InMemoryKeyValueStore<>;
using CountingKv = CountingKvStore<MemKv>;

StepOutput sleepStep(SleepEpisodeTracker& t, TimeMs t_ms) {
    return t.step(makeSnapshot(t_ms));
}

// Seeds a valid open marker (schema byte + previous-boot wall time), as an
// episode at promotion would have written it.
void seedMarker(MemKv& kv, uint8_t schema, bw::UnixSeconds unix_s) {
    OpenMarkerRecord rec{};
    rec.schema_version = schema;
    rec.t_start_unix = unix_s;
    const bw::Result<void> put = kv.put(kCfg.kMarkerKey, &rec, sizeof(rec));
    TEST_ASSERT_TRUE(put.ok());
}

bool markerIsCleared(MemKv& kv) {
    OpenMarkerRecord rec{};
    const bw::Result<size_t> got = kv.get(kCfg.kMarkerKey, &rec, sizeof(rec));
    return got.ok() && got.value == sizeof(rec) && rec.schema_version == 0;
}

}  // namespace

// ---- marker present ⇒ exactly one recovered event -----------------------------

void test_recovery_marker_present_emits_one_recovered_event(void) {
    MemKv kv;
    seedMarker(kv, kCfg.kMarkerSchemaVersion, kSyncedUnix);
    SleepEpisodeTracker t{kCfg, &kv, nullptr};

    StepOutput out = t.recover();
    TEST_ASSERT_TRUE(out.has_event);
    // §6 D4's recovered event: consumers learn "an episode was open before the
    // reboot; start/duration unknown" and nothing more.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::EventType::SleepEpisode),
                          static_cast<int>(out.event.type));
    TEST_ASSERT_EQUAL_UINT8(kCfg.kFlagRecovered,
                            out.event.payload[bw::sleep::SleepEpisodeConfig::kPayloadFlagsIndex]);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(EpisodeClass::Uncertain),
                            out.event.payload[bw::sleep::SleepEpisodeConfig::kPayloadClassIndex]);
    // The marker's share byte: 100 % of the episode is unobserved this boot.
    TEST_ASSERT_EQUAL_UINT8(100,
                            out.event.payload[bw::sleep::SleepEpisodeConfig::kPayloadShareIndex]);
    TEST_ASSERT_EQUAL_UINT64(0, out.event.t_start_ms);  // previous-boot monotonic times are a lie
    TEST_ASSERT_EQUAL_UINT32(0, out.event.duration_ms);
    TEST_ASSERT_EQUAL_UINT32(kSyncedUnix, out.event.t_start_unix);
    TEST_ASSERT_FLOAT_WITHIN(0.00001f, 0.0f, out.event.confidence);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(out.event.quality.imu));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                          static_cast<int>(out.event.quality.ppg));
    TEST_ASSERT_BITS(f08_test::reasonBit(bw::ReasonCode::SensorUnavailable),
                     f08_test::reasonBit(bw::ReasonCode::SensorUnavailable), out.event.reasons);
    // Recovery changes no live phase — the UI keeps Idle (§7: wiring writes
    // the mailbox only on status_changed).
    TEST_ASSERT_FALSE(out.status_changed);
    // Marker cleared by recover() (§9).
    TEST_ASSERT_TRUE(markerIsCleared(kv));
}

// ---- marker absent / corrupt / mismatched ⇒ silent ---------------------------

void test_recovery_marker_absent_no_event(void) {
    MemKv kv;  // empty store: the normal case, never a fault (§10)
    SleepEpisodeTracker t{kCfg, &kv, nullptr};
    TEST_ASSERT_FALSE(t.recover().has_event);
}

void test_recovery_truncated_marker_ignored(void) {
    MemKv kv;
    const uint8_t garbage[] = {0xA5, 0x5A, 0x01};  // shorter than the record
    const bw::Result<void> put = kv.put(kCfg.kMarkerKey, garbage, sizeof(garbage));
    TEST_ASSERT_TRUE(put.ok());
    SleepEpisodeTracker t{kCfg, &kv, nullptr};
    TEST_ASSERT_FALSE(t.recover().has_event);  // ignored, never decoded as garbage
}

void test_recovery_schema_mismatch_ignored(void) {
    MemKv kv;
    seedMarker(kv, /*schema=*/99, kSyncedUnix);
    SleepEpisodeTracker t{kCfg, &kv, nullptr};
    TEST_ASSERT_FALSE(t.recover().has_event);
}

// ---- idempotence and the W2 (kv == nullptr) build ------------------------------

void test_recovery_second_call_is_silent(void) {
    MemKv kv;
    seedMarker(kv, kCfg.kMarkerSchemaVersion, kSyncedUnix);
    SleepEpisodeTracker t{kCfg, &kv, nullptr};
    TEST_ASSERT_TRUE(t.recover().has_event);
    TEST_ASSERT_FALSE(t.recover().has_event);  // once per boot, even if called twice (§9)
}

void test_recovery_second_boot_after_recover_is_silent(void) {
    // A reboot right after a recovery must not re-emit: the marker was cleared.
    MemKv kv;
    seedMarker(kv, kCfg.kMarkerSchemaVersion, kSyncedUnix);
    SleepEpisodeTracker first_boot{kCfg, &kv, nullptr};
    TEST_ASSERT_TRUE(first_boot.recover().has_event);
    SleepEpisodeTracker second_boot{kCfg, &kv, nullptr};
    TEST_ASSERT_FALSE(second_boot.recover().has_event);
}

void test_recovery_kv_nullptr_no_event_no_crash(void) {
    // The exact wave-2 wiring (§13): no store at all — recover() is silent
    // and the machine keeps working.
    SleepEpisodeTracker t{kCfg, nullptr, nullptr};
    TEST_ASSERT_FALSE(t.recover().has_event);
    TEST_ASSERT_FALSE(t.recover().has_event);
    StepOutput started = sleepStep(t, kT0);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::EpisodePhase::Candidate),
                          static_cast<int>(started.status.phase));
}

// ---- marker cleared after every normal close ------------------------------------

void test_recovery_marker_cleared_after_normal_close(void) {
    CountingKv kv;
    SleepEpisodeTracker t{kCfg, &kv, nullptr};
    sleepStep(t, kT0);
    sleepStep(t, kT0 + kCfg.kMinEpisodeMs);  // Active: marker written
    StepOutput closed = t.step(awake(kT0 + kCfg.kMinEpisodeMs + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_TRUE(closed.has_event);
    TEST_ASSERT_TRUE(markerIsCleared(kv.inner));
    // A following boot finds nothing to recover (a stale marker would
    // re-trigger recovery for an episode that already ended, §10).
    SleepEpisodeTracker next_boot{kCfg, &kv, nullptr};
    TEST_ASSERT_FALSE(next_boot.recover().has_event);
}

// ---- put-count budget: 1 open + 1 close per Active episode -----------------------

void test_recovery_put_count_open_and_close_per_episode(void) {
    CountingKv kv;
    SleepEpisodeTracker t{kCfg, &kv, nullptr};
    // A candidate that aborts costs ZERO writes (§6 D4: blips never touch flash).
    sleepStep(t, kT0);
    t.step(awake(kT0 + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(kv.put_count));
    // A full Active episode costs exactly two: the open marker at promotion
    // and the clear at close.
    const TimeMs promote_t = kT0 + 200000;
    sleepStep(t, promote_t);
    sleepStep(t, promote_t + kCfg.kMinEpisodeMs);
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(kv.put_count));
    t.step(awake(promote_t + kCfg.kMinEpisodeMs + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(kv.put_count));
}

void test_recovery_put_failure_at_promotion_still_active(void) {
    // §10: storage failure at promotion degrades recovery only — the episode
    // still becomes Active and events keep flowing; no crash, no fault.
    CountingKv kv;
    kv.fail_puts = true;
    SleepEpisodeTracker t{kCfg, &kv, nullptr};
    sleepStep(t, kT0);
    StepOutput promoted = sleepStep(t, kT0 + kCfg.kMinEpisodeMs);
    TEST_ASSERT_TRUE(promoted.has_event);  // the open marker still goes out
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::EpisodePhase::Active),
                          static_cast<int>(promoted.status.phase));
    StepOutput closed = t.step(awake(kT0 + kCfg.kMinEpisodeMs + kCfg.kMergeGapMs + 1));
    TEST_ASSERT_TRUE(closed.has_event);  // and the close still completes
}
