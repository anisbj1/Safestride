#pragma once

// Scripted II2cBackend fake — private test helper for the bw_hal suite
// (host-testing guidance: private helpers live under the feature's own test/).
// Records every call so tests can assert lock/unlock pairing, transfer counts
// (retry and fail-fast behaviour) and recovery escalation.

#include <cstddef>
#include <cstdint>

#include <bw/result.h>
#include <bw/types.h>

#include <bw/hal/i2c_backend.h>

namespace bw::hal::test {

class FakeI2cBackend final : public II2cBackend {
public:
    static constexpr size_t kMaxScript = 32;
    static constexpr size_t kMaxCalls = 64;

    struct TransferCall {
        uint8_t addr7;
        size_t tx_len;
        size_t rx_len;
        bw::Err err;
    };

    // --- scripting -----------------------------------------------------------
    // Outcomes are consumed one per transfer(); when the script is exhausted,
    // `default_err` is returned. Scripting "Timeouts then Ok" models an outage
    // that recovers; leaving the default Ok models a healthy bus.
    bool pushOutcome(bw::Err err) {
        if (script_len_ >= kMaxScript) {
            return false;
        }
        script_[script_len_++] = err;
        return true;
    }
    void setDefault(bw::Err err) { default_err_ = err; }
    void setLockResult(bool ok) { lock_ok_ = ok; }
    void setRecoverResult(bool ok) { recover_result_ = ok; }

    // --- assertions support --------------------------------------------------
    size_t lockCount() const { return lock_count_; }
    size_t unlockCount() const { return unlock_count_; }
    size_t recoverCount() const { return recover_count_; }
    size_t transferCount() const { return transfer_count_; }
    bw::TimeMs lastLockTimeoutMs() const { return last_lock_timeout_ms_; }
    bw::TimeMs lastTransferTimeoutMs() const { return last_transfer_timeout_ms_; }
    const TransferCall& callAt(size_t i) const { return calls_[i]; }

    // --- II2cBackend ---------------------------------------------------------
    bool lock(bw::TimeMs timeout_ms) override {
        ++lock_count_;
        last_lock_timeout_ms_ = timeout_ms;
        return lock_ok_;
    }
    void unlock() override { ++unlock_count_; }

    bw::Err transfer(uint8_t addr7, const uint8_t*, size_t tx_len, uint8_t*, size_t rx_len,
                     bw::TimeMs timeout_ms) override {
        bw::Err err = default_err_;
        if (script_pos_ < script_len_) {
            err = script_[script_pos_++];
        }
        if (transfer_count_ < kMaxCalls) {
            calls_[transfer_count_] = TransferCall{addr7, tx_len, rx_len, err};
        }
        ++transfer_count_;
        last_transfer_timeout_ms_ = timeout_ms;
        return err;
    }

    bool recover() override {
        ++recover_count_;
        return recover_result_;
    }

private:
    bw::Err script_[kMaxScript] = {};
    size_t script_pos_ = 0;
    size_t script_len_ = 0;
    bw::Err default_err_ = bw::Err::Ok;

    TransferCall calls_[kMaxCalls] = {};
    size_t transfer_count_ = 0;

    bool lock_ok_ = true;
    bool recover_result_ = true;
    size_t lock_count_ = 0;
    size_t unlock_count_ = 0;
    size_t recover_count_ = 0;
    bw::TimeMs last_lock_timeout_ms_ = 0;
    bw::TimeMs last_transfer_timeout_ms_ = 0;
};

}  // namespace bw::hal::test
