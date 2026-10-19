// bw_reliability — FaultRegistry tests (spec §16.8 item 1): per-code records,
// the sticky D4 bit mapping (bit N <=> FaultCode N), defined no-ops and
// deterministic interleavings.

#include <bw/fault.h>
#include <bw/reliability/fault_registry.h>
#include <bw/reliability/reliability_config.h>

#include <unity.h>

namespace {

constexpr bw::TimeMs kT = 1000;

uint32_t bitOf(const bw::FaultCode code) {
    return 1u << static_cast<uint8_t>(code);
}

}  // namespace

// First-call empty state: record() reports "no record", never a fabricated
// zero (00 §1) — then one report fills count/first/last exactly.
void test_registry_first_report_fills_empty_state(void) {
    bw::reliability::FaultRegistry reg;
    bw::reliability::FaultRecord rec{};
    TEST_ASSERT_FALSE(reg.record(bw::FaultCode::ImuReadFailed, rec));
    TEST_ASSERT_FALSE(reg.has(bw::FaultCode::ImuReadFailed));
    TEST_ASSERT_EQUAL_UINT32(0, reg.faultBits());

    reg.report(bw::FaultCode::ImuReadFailed, bw::FaultSeverity::Error, kT);
    TEST_ASSERT_TRUE(reg.has(bw::FaultCode::ImuReadFailed));
    TEST_ASSERT_TRUE(reg.record(bw::FaultCode::ImuReadFailed, rec));
    TEST_ASSERT_EQUAL_UINT32(1, rec.count);
    TEST_ASSERT_EQUAL_UINT64(kT, rec.first_ms);
    TEST_ASSERT_EQUAL_UINT64(kT, rec.last_ms);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultSeverity::Error),
                          static_cast<int>(rec.max_severity));
}

// Counts track first/last across repeats and severity takes the MAX, never a
// later-latched lower one (severity order = enum order, fault.h).
void test_registry_counts_first_last_and_max_severity(void) {
    bw::reliability::FaultRegistry reg;
    reg.report(bw::FaultCode::PpgFlatline, bw::FaultSeverity::Error, 100);
    reg.report(bw::FaultCode::PpgFlatline, bw::FaultSeverity::Info, 200);
    reg.report(bw::FaultCode::PpgFlatline, bw::FaultSeverity::Warning, 300);

    bw::reliability::FaultRecord rec{};
    TEST_ASSERT_TRUE(reg.record(bw::FaultCode::PpgFlatline, rec));
    TEST_ASSERT_EQUAL_UINT32(3, rec.count);
    TEST_ASSERT_EQUAL_UINT64(100, rec.first_ms);
    TEST_ASSERT_EQUAL_UINT64(300, rec.last_ms);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultSeverity::Error),
                          static_cast<int>(rec.max_severity));
}

// D4 golden: bit N <=> FaultCode value N — the exact word F23's CR-030
// consumer decodes with the same mapping.
void test_registry_sticky_bits_follow_bit_n_code_n(void) {
    bw::reliability::FaultRegistry reg;
    reg.report(bw::FaultCode::TaskStalled, bw::FaultSeverity::Critical, kT);
    reg.report(bw::FaultCode::BleDown, bw::FaultSeverity::Warning, kT + 1);

    const uint32_t bits = reg.faultBits();
    TEST_ASSERT_EQUAL_HEX32(bitOf(bw::FaultCode::TaskStalled) | bitOf(bw::FaultCode::BleDown),
                            bits);
    // Golden: StorageDegraded is code 23, BleDown 24 (fault.h, spec row 3/4).
    TEST_ASSERT_EQUAL_HEX32(1u << 23, bitOf(bw::FaultCode::StorageDegraded));
    TEST_ASSERT_EQUAL_HEX32(1u << 24, bitOf(bw::FaultCode::BleDown));
    // Distinct codes only.
    TEST_ASSERT_EQUAL_UINT32(2, reg.distinctCodes());
}

// FaultCode::None is the "no fault" sentinel and codes >= kMaxFaultCodes are
// out of range: both are defined no-ops (F23 I2), never UB, never a bit set.
void test_registry_none_and_out_of_range_are_noops(void) {
    bw::reliability::FaultRegistry reg;
    reg.report(bw::FaultCode::None, bw::FaultSeverity::Critical, kT);
    reg.report(static_cast<bw::FaultCode>(bw::reliability::kMaxFaultCodes),
               bw::FaultSeverity::Critical, kT);
    reg.report(static_cast<bw::FaultCode>(31), bw::FaultSeverity::Critical, kT);

    TEST_ASSERT_EQUAL_UINT32(0, reg.faultBits());
    TEST_ASSERT_EQUAL_UINT32(0, reg.distinctCodes());
    TEST_ASSERT_FALSE(reg.has(bw::FaultCode::None));
}

// CR-033 boundary pin: codes 27 (TaskCreateFailed) and 28 (EventPublishDrop)
// are the newest DEFINED codes and must be fully in range — count, record and
// bit — while kMaxFaultCodes must equal "last defined code + 1". Reverting
// kMaxFaultCodes to 27 makes both reports defined no-ops and fails here.
void test_registry_cr033_codes_are_in_range(void) {
    bw::reliability::FaultRegistry reg;
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(bw::FaultCode::EventPublishDrop) + 1,
                             bw::reliability::kMaxFaultCodes);

    reg.report(bw::FaultCode::TaskCreateFailed, bw::FaultSeverity::Critical, kT);
    reg.report(bw::FaultCode::EventPublishDrop, bw::FaultSeverity::Warning, kT + 1);

    TEST_ASSERT_TRUE(reg.has(bw::FaultCode::TaskCreateFailed));
    bw::reliability::FaultRecord rec{};
    TEST_ASSERT_TRUE(reg.record(bw::FaultCode::EventPublishDrop, rec));
    TEST_ASSERT_EQUAL_UINT32(1, rec.count);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::FaultSeverity::Warning),
                          static_cast<int>(rec.max_severity));
    TEST_ASSERT_EQUAL_HEX32(bitOf(bw::FaultCode::TaskCreateFailed) |
                                bitOf(bw::FaultCode::EventPublishDrop),
                            reg.faultBits());
    TEST_ASSERT_EQUAL_UINT32(2, reg.distinctCodes());
}

// Sticky: once a bit is set it never clears this boot — a later "recovery"
// report cannot undo history (D4: no FaultCleared code exists).
void test_registry_bits_are_sticky(void) {
    bw::reliability::FaultRegistry reg;
    reg.report(bw::FaultCode::ImuAbsent, bw::FaultSeverity::Error, kT);
    reg.report(bw::FaultCode::ImuAbsent, bw::FaultSeverity::Info, kT + 5000);
    TEST_ASSERT_TRUE(reg.has(bw::FaultCode::ImuAbsent));
    TEST_ASSERT_EQUAL_HEX32(bitOf(bw::FaultCode::ImuAbsent), reg.faultBits());
    TEST_ASSERT_EQUAL_UINT32(1, reg.distinctCodes());
}

// Deterministic cross-task interleaving (two reporters alternating): each
// code keeps its own count and timestamps, no cross-talk.
void test_registry_interleaved_reports_stay_per_code(void) {
    bw::reliability::FaultRegistry reg;
    reg.report(bw::FaultCode::TaskOverrun, bw::FaultSeverity::Warning, 100);
    reg.report(bw::FaultCode::LowHeap, bw::FaultSeverity::Error, 150);
    reg.report(bw::FaultCode::TaskOverrun, bw::FaultSeverity::Warning, 200);
    reg.report(bw::FaultCode::LowHeap, bw::FaultSeverity::Error, 250);

    bw::reliability::FaultRecord overrun{};
    bw::reliability::FaultRecord low_heap{};
    TEST_ASSERT_TRUE(reg.record(bw::FaultCode::TaskOverrun, overrun));
    TEST_ASSERT_TRUE(reg.record(bw::FaultCode::LowHeap, low_heap));
    TEST_ASSERT_EQUAL_UINT32(2, overrun.count);
    TEST_ASSERT_EQUAL_UINT64(100, overrun.first_ms);
    TEST_ASSERT_EQUAL_UINT64(200, overrun.last_ms);
    TEST_ASSERT_EQUAL_UINT32(2, low_heap.count);
    TEST_ASSERT_EQUAL_UINT64(150, low_heap.first_ms);
    TEST_ASSERT_EQUAL_UINT64(250, low_heap.last_ms);
    TEST_ASSERT_EQUAL_UINT32(2, reg.distinctCodes());
}

// The registry IS an IFaultReporter implementation (D3): a base-pointer call
// must behave exactly like the concrete one — that is how AppContext-bound
// features report (D14).
void test_registry_works_through_iface_pointer(void) {
    bw::reliability::FaultRegistry reg;
    bw::IFaultReporter* reporter = &reg;
    reporter->report(bw::FaultCode::WatchdogReset, bw::FaultSeverity::Critical, kT);
    TEST_ASSERT_TRUE(reg.has(bw::FaultCode::WatchdogReset));
    TEST_ASSERT_EQUAL_HEX32(bitOf(bw::FaultCode::WatchdogReset), reg.faultBits());
}
