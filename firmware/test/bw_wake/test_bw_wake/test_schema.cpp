// test/bw_wake/test_bw_wake/test_schema.cpp — FEATURE_SCHEMA §2.1 rules as
// they apply to F06's alertness feature set (the header's static_assert
// covers the same ground at compile time; this pins the contract in the
// test report and documents it for reviewers).

#include <unity.h>

#include <cstring>

#include <bw/wake/alertness_schema.h>

#include "wake_test_support.h"

using namespace bw;
using bw::wake::kAlertnessFeatureCount;
using bw::wake::kAlertnessFeatureMean;
using bw::wake::kAlertnessFeatureNames;
using bw::wake::kAlertnessFeatureReason;
using bw::wake::kAlertnessFeatureStd;
using bw::wake::kAlertnessSchemaId;
using bw::wake::kAlertnessSchemaVersion;
using bw::wake::kNoFeatureReason;

namespace {

// Indices whose schema entry is `normalization.method == "none"`: identity
// (mean 0, std 1), so the raw value passes through unchanged.
const uint8_t kIdentityFeatures[] = {1, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

}  // namespace

void test_schema_ids_are_never_zero(void) {
    // FEATURE_SCHEMA §2.1: 0 means "invalid, never issued".
    TEST_ASSERT_TRUE(kAlertnessSchemaId != 0);
    TEST_ASSERT_TRUE(kAlertnessSchemaVersion != 0);
    TEST_ASSERT_TRUE(kAlertnessSchemaId <= 0xFFFFu);
}

void test_schema_fits_the_static_bound(void) {
    TEST_ASSERT_TRUE(kAlertnessFeatureCount > 0);
    TEST_ASSERT_TRUE(kAlertnessFeatureCount <= kMaxFeatures);
}

void test_schema_names_are_unique_and_snake_case(void) {
    for (uint8_t i = 0; i < kAlertnessFeatureCount; ++i) {
        const char* name = kAlertnessFeatureNames[i];
        TEST_ASSERT_TRUE(name != nullptr && name[0] != '\0');
        for (uint8_t c = 0; name[c] != '\0'; ++c) {
            const char ch = name[c];
            const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
            TEST_ASSERT_TRUE_MESSAGE(ok, "feature names are snake_case");
        }
        for (uint8_t j = static_cast<uint8_t>(i + 1); j < kAlertnessFeatureCount; ++j) {
            TEST_ASSERT_TRUE_MESSAGE(std::strcmp(name, kAlertnessFeatureNames[j]) != 0,
                                     "names are unique within the schema (§2.1)");
        }
    }
}

void test_schema_normalization_is_sane(void) {
    for (uint8_t i = 0; i < kAlertnessFeatureCount; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(kAlertnessFeatureStd[i] > 0.0f, "std > 0 (§2.1)");
        // Rejects NaN and Inf as well as absurd values (no clinical range is
        // in scope — 00 §1 — only "not a corrupted constant").
        const float mean = kAlertnessFeatureMean[i];
        TEST_ASSERT_TRUE(mean > -1.0e9f && mean < 1.0e9f);
    }
}

void test_schema_none_method_is_identity(void) {
    for (size_t i = 0; i < sizeof(kIdentityFeatures) / sizeof(kIdentityFeatures[0]); ++i) {
        const uint8_t index = kIdentityFeatures[i];
        TEST_ASSERT_TRUE(index < kAlertnessFeatureCount);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, kAlertnessFeatureMean[index]);
        TEST_ASSERT_EQUAL_FLOAT(1.0f, kAlertnessFeatureStd[index]);
    }
}

void test_schema_reason_table_only_holds_real_codes(void) {
    for (uint8_t i = 0; i < kAlertnessFeatureCount; ++i) {
        const uint8_t code = kAlertnessFeatureReason[i];
        // Either "no reason for this feature" or a code that fits the
        // ReasonMask bitset (bw/state.h uses uint32).
        const bool ok = code == kNoFeatureReason || code < 32;
        TEST_ASSERT_TRUE_MESSAGE(ok, "reason code fits ReasonMask");
    }
}
