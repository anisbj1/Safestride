// F00 §9: providers — IStateProvider/IFeatureProvider "no data yet" -> false, and the
// CR-003 ISettings semantics (accepted 2026-09-30): registry defaults, min/max
// CLAMPING (not rejection), unknown key -> Err::NotFound, type mismatch -> Err::InvalidArg.
// CR-006 (accepted 2026-10-01) pins list(): TOTAL registered count returned, at most
// `max` entries filled, so `returned > max` means truncated (services.md §5).
//
// The doubles here are test-local on purpose: SettingKey is deliberately empty at M0
// (CR-003 — each feature registers its own keys when it lands) and no production
// ISettings implementation exists yet, so shipping a reference fake in bw_testkit
// would pre-commit F00 to storage decisions that belong to F18/F19. If a second
// feature needs these doubles, promote them to bw_testkit then.

#include <bw/providers.h>

#include <bw/containers/latest_value.h>

#include <unity.h>

#include <cstddef>
#include <cstdint>

namespace {

// SettingKey is empty by design (CR-003); these casts are test-only labels and add
// nothing to the enum — the append-only guard lives in test_contract_layout.cpp.
constexpr bw::SettingKey kIntKey = static_cast<bw::SettingKey>(1);
constexpr bw::SettingKey kFloatKey = static_cast<bw::SettingKey>(2);
constexpr bw::SettingKey kMissingKey = static_cast<bw::SettingKey>(0xBEEF);

bw::SettingValue intVal(int32_t v) {
    bw::SettingValue x{};
    x.type = bw::SettingType::Int;
    x.i = v;
    return x;
}

bw::SettingValue floatVal(float v) {
    bw::SettingValue x{};
    x.type = bw::SettingType::Float;
    x.f = v;
    return x;
}

// One entry of the registration table: the spec plus the current value (which the
// constructor seeds from the spec's default, so get() before any set() returns it).
struct Entry {
    bw::SettingSpec spec;
    bw::SettingValue current;
};

// Reference ISettings double implementing CR-003 exactly. Fixed table, no heap
// (00 §7.2), registration fixed at construction as CR-003 requires.
class TestSettings : public bw::ISettings {
public:
    // Why 2: one entry per value type the tests exercise (Int, Float) — the table is
    // a test fixture, not a product limit.
    static constexpr size_t kMaxEntries = 2;

    TestSettings() {
        entries_[0].spec = bw::SettingSpec{kIntKey,     bw::SettingType::Int, intVal(0),
                                           intVal(100), intVal(50),           "test_int"};
        entries_[1].spec = bw::SettingSpec{kFloatKey,      bw::SettingType::Float, floatVal(0.0f),
                                           floatVal(1.0f), floatVal(0.25f),        "test_float"};
        for (size_t i = 0; i < kMaxEntries; ++i) {
            entries_[i].current = entries_[i].spec.default_v;
        }
    }

    bw::Result<bw::SettingValue> get(bw::SettingKey key) const override {
        const Entry* e = find(key);
        if (e == nullptr) {
            // value is a default object on the error path (D5); explicitly
            // value-initialized so -Wmissing-field-initializers stays quiet.
            return {bw::Err::NotFound, bw::SettingValue{}};
        }
        return {bw::Err::Ok, e->current};
    }
    bw::Result<void> set(bw::SettingKey key, const bw::SettingValue& value) override {
        Entry* e = find(key);
        if (e == nullptr) {
            return {bw::Err::NotFound};
        }
        if (value.type != e->spec.type) {
            return {bw::Err::InvalidArg};  // wrong typed member read -> refuse (CR-003)
        }
        e->current = clamp(e->spec, value);  // outside min..max -> CLAMPED, not rejected
        return {bw::Err::Ok};
    }

    // CR-006 (accepted 2026-10-01): returns the TOTAL registered count, fills at most
    // `max` entries, first `max` in registration order — so `returned > max` signals
    // "truncated", snprintf()-style. The previous count-written convention was wrong.
    size_t list(bw::SettingSpec* out, size_t max) const override {
        size_t n = (count_ < max) ? count_ : max;
        for (size_t i = 0; i < n; ++i) {
            out[i] = entries_[i].spec;
        }
        return count_;
    }

private:
    Entry* find(bw::SettingKey key) {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].spec.key == key) {
                return &entries_[i];
            }
        }
        return nullptr;
    }

    const Entry* find(bw::SettingKey key) const {
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i].spec.key == key) {
                return &entries_[i];
            }
        }
        return nullptr;
    }

    // Caller has already checked value.type == spec.type, so min/max/default_v are
    // read in the matching member (registration invariant: same type for all four).
    static bw::SettingValue clamp(const bw::SettingSpec& spec, const bw::SettingValue& v) {
        bw::SettingValue out = v;
        switch (v.type) {
        case bw::SettingType::Int:
            if (out.i < spec.min.i) {
                out.i = spec.min.i;
            }
            if (out.i > spec.max.i) {
                out.i = spec.max.i;
            }
            break;
        case bw::SettingType::Float:
            if (out.f < spec.min.f) {
                out.f = spec.min.f;
            }
            if (out.f > spec.max.f) {
                out.f = spec.max.f;
            }
            break;
        case bw::SettingType::Bool:
            break;  // a bool has no range to clamp
        }
        return out;
    }

    Entry entries_[kMaxEntries]{};
    size_t count_ = kMaxEntries;
};

// Provider doubles: exactly the LatestValue wiring F07/F23 will use, so "no data yet"
// is exercised through the real interface instead of a hand-rolled flag.
class TestStateProvider : public bw::IStateProvider {
public:
    void publish(const bw::StateSnapshot& v) { box_.set(v); }

    bool latest(bw::StateSnapshot& out) const override { return box_.get(out); }

private:
    bw::LatestValue<bw::StateSnapshot> box_;
};

class TestFeatureProvider : public bw::IFeatureProvider {
public:
    void publishMotion(const bw::MotionFeatures& v) { motion_.set(v); }
    void publishPpg(const bw::PpgFeatures& v) { ppg_.set(v); }

    bool latestMotion(bw::MotionFeatures& out) const override { return motion_.get(out); }
    bool latestPpg(bw::PpgFeatures& out) const override { return ppg_.get(out); }

private:
    bw::LatestValue<bw::MotionFeatures> motion_;
    bw::LatestValue<bw::PpgFeatures> ppg_;
};

}  // namespace

void test_state_provider_no_data_until_first_publish(void) {
    TestStateProvider provider;
    bw::StateSnapshot out{};
    out.t_ms = 999;  // sentinel: get() must not touch out when there is no data

    TEST_ASSERT_FALSE(provider.latest(out));
    TEST_ASSERT_EQUAL_UINT64(999, out.t_ms);  // untouched, never a fabricated snapshot
}

void test_state_provider_returns_newest_snapshot(void) {
    TestStateProvider provider;
    bw::StateSnapshot first{};
    first.t_ms = 1000;
    first.alertness = bw::AlertnessState::Awake;
    bw::StateSnapshot second{};
    second.t_ms = 2000;
    second.alertness = bw::AlertnessState::Drowsy;

    provider.publish(first);
    provider.publish(second);  // intermediate dropped by design (newest wins)

    bw::StateSnapshot out{};
    TEST_ASSERT_TRUE(provider.latest(out));
    TEST_ASSERT_EQUAL_UINT64(2000, out.t_ms);
    TEST_ASSERT_EQUAL_INT(bw::AlertnessState::Drowsy, out.alertness);
}

void test_feature_provider_no_data_until_publish(void) {
    TestFeatureProvider provider;
    bw::MotionFeatures motion{};
    bw::PpgFeatures ppg{};
    motion.t_ms = 999;
    ppg.t_ms = 999;

    TEST_ASSERT_FALSE(provider.latestMotion(motion));
    TEST_ASSERT_FALSE(provider.latestPpg(ppg));
    TEST_ASSERT_EQUAL_UINT64(999, motion.t_ms);  // both untouched while empty
    TEST_ASSERT_EQUAL_UINT64(999, ppg.t_ms);
}

void test_feature_provider_channels_are_independent(void) {
    TestFeatureProvider provider;
    bw::MotionFeatures published{};
    published.t_ms = 1000;
    published.enmo_g = 0.03f;
    provider.publishMotion(published);

    bw::MotionFeatures motion{};
    bw::PpgFeatures ppg{};
    TEST_ASSERT_TRUE(provider.latestMotion(motion));
    TEST_ASSERT_EQUAL_UINT64(1000, motion.t_ms);
    TEST_ASSERT_FALSE(provider.latestPpg(ppg));  // PPG channel still empty
}

void test_settings_unregistered_key_returns_not_found(void) {
    TestSettings settings;

    bw::Result<bw::SettingValue> read = settings.get(kMissingKey);
    TEST_ASSERT_FALSE(read.ok());
    TEST_ASSERT_EQUAL_INT(bw::Err::NotFound, read.err);

    bw::Result<void> write = settings.set(kMissingKey, intVal(10));
    TEST_ASSERT_FALSE(write.ok());
    TEST_ASSERT_EQUAL_INT(bw::Err::NotFound, write.err);
}

void test_settings_get_returns_registered_default(void) {
    TestSettings settings;

    // Before any set(): the registry default, not a magic value (02 §1)
    bw::Result<bw::SettingValue> i = settings.get(kIntKey);
    TEST_ASSERT_TRUE(i.ok());
    TEST_ASSERT_EQUAL_INT(bw::SettingType::Int, i.value.type);
    TEST_ASSERT_EQUAL_INT32(50, i.value.i);

    bw::Result<bw::SettingValue> f = settings.get(kFloatKey);
    TEST_ASSERT_TRUE(f.ok());
    TEST_ASSERT_EQUAL_INT(bw::SettingType::Float, f.value.type);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, f.value.f);
}

void test_settings_type_mismatch_returns_invalid_arg(void) {
    TestSettings settings;
    TEST_ASSERT_TRUE(settings.set(kIntKey, intVal(42)).ok());

    // A Float-typed value on an Int key must be refused BEFORE any member is read,
    // otherwise set() would clamp against the wrong union member (CR-003).
    bw::Result<void> bad = settings.set(kIntKey, floatVal(0.5f));
    TEST_ASSERT_FALSE(bad.ok());
    TEST_ASSERT_EQUAL_INT(bw::Err::InvalidArg, bad.err);

    bw::Result<bw::SettingValue> read = settings.get(kIntKey);
    TEST_ASSERT_EQUAL_INT32(42, read.value.i);  // stored value untouched by the refusal
}

void test_settings_out_of_range_is_clamped_not_rejected(void) {
    TestSettings settings;

    bw::Result<void> hi = settings.set(kIntKey, intVal(500));
    TEST_ASSERT_TRUE(hi.ok());  // CR-003: clamped, NOT rejected
    TEST_ASSERT_EQUAL_INT32(100, settings.get(kIntKey).value.i);

    bw::Result<void> lo = settings.set(kIntKey, intVal(-7));
    TEST_ASSERT_TRUE(lo.ok());
    TEST_ASSERT_EQUAL_INT32(0, settings.get(kIntKey).value.i);

    bw::Result<void> fhi = settings.set(kFloatKey, floatVal(4.0f));
    TEST_ASSERT_TRUE(fhi.ok());
    TEST_ASSERT_EQUAL_FLOAT(1.0f, settings.get(kFloatKey).value.f);
}

void test_settings_in_range_stored_verbatim(void) {
    TestSettings settings;

    TEST_ASSERT_TRUE(settings.set(kIntKey, intVal(42)).ok());
    TEST_ASSERT_EQUAL_INT32(42, settings.get(kIntKey).value.i);

    TEST_ASSERT_TRUE(settings.set(kFloatKey, floatVal(0.5f)).ok());
    TEST_ASSERT_EQUAL_FLOAT(0.5f, settings.get(kFloatKey).value.f);
}

void test_settings_list_returns_range_table(void) {
    TestSettings settings;
    bw::SettingSpec out[TestSettings::kMaxEntries]{};

    size_t n = settings.list(out, TestSettings::kMaxEntries);
    TEST_ASSERT_EQUAL_UINT(TestSettings::kMaxEntries, n);
    TEST_ASSERT_EQUAL_INT(kIntKey, out[0].key);
    TEST_ASSERT_EQUAL_INT(bw::SettingType::Int, out[0].type);
    TEST_ASSERT_EQUAL_INT32(0, out[0].min.i);
    TEST_ASSERT_EQUAL_INT32(100, out[0].max.i);
    TEST_ASSERT_EQUAL_INT32(50, out[0].default_v.i);
    TEST_ASSERT_EQUAL_STRING("test_int", out[0].name);
    TEST_ASSERT_EQUAL_INT(kFloatKey, out[1].key);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, out[1].max.f);
}

void test_settings_list_truncates_at_max_but_returns_total(void) {
    TestSettings settings;
    bw::SettingSpec out[TestSettings::kMaxEntries]{};
    // Sentinel on the slot the truncated call must NOT fill: list() writing past
    // `max` would corrupt the caller's buffer.
    out[1].key = kMissingKey;

    // CR-006: max (1) < registered (2) -> fills only the first entry, in registration
    // order, but returns the TOTAL registered count so `returned > max` signals
    // "truncated — call again with a bigger buffer" (snprintf convention).
    constexpr size_t kMax = 1;
    size_t n = settings.list(out, kMax);

    TEST_ASSERT_EQUAL_UINT(TestSettings::kMaxEntries, n);  // total, not written count
    TEST_ASSERT_TRUE(n > kMax);                            // truncation is detectable
    TEST_ASSERT_EQUAL_INT(kIntKey, out[0].key);            // first `max`, registration order
    TEST_ASSERT_EQUAL_INT(kMissingKey, out[1].key);        // untouched beyond `max`
}
