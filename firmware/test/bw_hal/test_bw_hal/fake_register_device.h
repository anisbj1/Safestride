#pragma once

// Register-level II2cBackend fake — private test helper for the bw_hal suite
// (host-testing guidance: private helpers live under the feature's own test/).
//
// The scripted FakeI2cBackend in fake_i2c_backend.h only returns error codes;
// driver tests need register *semantics*: Mpu6050Imu and Max30102Ppg write
// configuration registers and read bursts, so this fake models up to two
// devices as register files (plus a byte FIFO queue for the MAX30102 sample
// stream) and still records every transfer for "no bus traffic" assertions.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <bw/result.h>
#include <bw/types.h>

#include <bw/hal/i2c_backend.h>

namespace bw::hal::test {

class FakeRegisterDevice {
public:
    static constexpr size_t kRegCount = 256;
    static constexpr size_t kFifoCapacity = 256;

    // Register file: writes land here, reads come from here (except FIFO_DATA).
    uint8_t regs[kRegCount] = {};
    // MAX30102 FIFO_DATA emulation: reads at kFifoDataReg pop from this queue
    // oldest-first and advance the emulated read pointer (5-bit wrap).
    uint8_t fifo[kFifoCapacity] = {};
    size_t fifo_len = 0;
    // Error injection: nack = device absent (NACK), err = bus-level outcome.
    bool nack = false;
    bw::Err err = bw::Err::Ok;
    // 2026-10-13 bench truth set (hardware loop 1, evidence
    // ppg_first_light_20261013.md + loop-2 soak): the clone ignores some
    // writes. Each knob models one observed/possible silicon behavior; the
    // driver must adapt via readback under every combination.
    // - ovf_write_ignored: writes to the overflow-counter register are dropped
    //   (observed: OVF_PTR read >0 for 45+ s despite a zero write each drain).
    bool ovf_write_ignored = false;
    // - pointer_writes_ignored: writes to FIFO_WR_PTR/FIFO_RD_PTR dropped
    //   (nothing but silicon-internal logic can re-sync the pointers).
    bool pointer_writes_ignored = false;
    // - reset_ignored: the MODE_CONFIG RESET write is dropped entirely (the
    //   register keeps its old value, so a bit poll reads "already clear" and
    //   a naive driver believes the reset happened).
    bool reset_ignored = false;
    // - fifo_pop_broken: FIFO_DATA reads return bytes but do NOT advance the
    //   read pointer (no software path can drain the FIFO).
    bool fifo_pop_broken = false;
    // MODE_CONFIG RESET modelling (when not ignored): while the bit reads 1
    // the chip is resetting; when it clears, applyPowerOnDefaults() runs.
    // reset_hold_polls keeps the bit set for that many MODE_CONFIG reads
    // (0 = clears on the next poll), so tests can prove begin() polls until
    // completion instead of writing config registers into a chip that is
    // about to wipe them.
    uint8_t reset_hold_polls = 0;
    bool reset_pending = false;
    uint8_t reset_polls_left = 0;

    // Chip-level reset: every register returns to its power-on default —
    // pointers/overflow zeroed, MODE = shutdown, the sample FIFO emptied.
    void applyPowerOnDefaults() {
        regs[kModeConfigReg] = 0x00;
        regs[0x04] = 0x00;  // FIFO_WR_PTR
        regs[0x05] = 0x00;  // OVF_PTR
        regs[0x06] = 0x00;  // FIFO_RD_PTR
        fifo_len = 0;
        reset_pending = false;
    }

    // [HW] MODE_CONFIG (datasheet register map): bit 6 = RESET, self-clearing.
    static constexpr uint8_t kModeConfigReg = 0x09;
    static constexpr uint8_t kModeResetBit = 0x40;
};

class FakeRegisterBus final : public II2cBackend {
public:
    static constexpr size_t kMaxDevices = 2;
    static constexpr size_t kMaxTransfers = 64;

    struct Transfer {
        uint8_t addr7;
        size_t tx_len;
        size_t rx_len;
        bw::Err err;
    };

    // First use of an address creates the device slot (like hardware showing
    // up on the bus). Returns the device so tests can seed registers/FIFO.
    FakeRegisterDevice& device(uint8_t addr7) {
        for (size_t i = 0; i < device_count_; ++i) {
            if (addrs_[i] == addr7) {
                return devices_[i];
            }
        }
        addrs_[device_count_] = addr7;
        return devices_[device_count_++];
    }

    size_t transferCount() const { return transfer_count_; }
    const Transfer& callAt(size_t i) const { return transfers_[i]; }
    size_t recoverCount() const { return recover_count_; }

    // II2cBackend — lock contention and recovery policy are I2cBus concerns
    // already covered by test_i2c_bus.cpp; this fake always succeeds there.
    bool lock(bw::TimeMs) override { return true; }
    void unlock() override {}
    bool recover() override {
        ++recover_count_;
        return true;
    }

    bw::Err transfer(uint8_t addr7, const uint8_t* tx, size_t tx_len, uint8_t* rx, size_t rx_len,
                     bw::TimeMs) override {
        if (transfer_count_ < kMaxTransfers) {
            transfers_[transfer_count_] = Transfer{addr7, tx_len, rx_len, bw::Err::Ok};
        }
        ++transfer_count_;

        FakeRegisterDevice* dev = find(addr7);
        bw::Err err = bw::Err::Ok;
        if (dev == nullptr || dev->nack) {
            err = bw::Err::NotFound;  // nobody answers at that address
        } else if (dev->err != bw::Err::Ok) {
            err = dev->err;  // injected bus-level failure (timeout etc.)
        }
        if (transfer_count_ <= kMaxTransfers) {
            transfers_[transfer_count_ - 1].err = err;
        }
        if (err != bw::Err::Ok) {
            return err;
        }

        // Write phase: tx[0] = start register, tx[1..] = values written
        // sequentially (the i2c "register write" convention the drivers use).
        if (tx_len >= 1 && tx != nullptr) {
            const uint8_t start = tx[0];
            for (size_t i = 1; i < tx_len; ++i) {
                const uint8_t addr = static_cast<uint8_t>(start + i - 1);
                if (dev->reset_ignored && addr == FakeRegisterDevice::kModeConfigReg &&
                    (tx[i] & FakeRegisterDevice::kModeResetBit) != 0) {
                    continue;  // clone drops the RESET write: the register keeps its old value
                }
                if (dev->ovf_write_ignored && addr == 0x05) {
                    continue;  // silicon: the overflow counter resists this write
                }
                if (dev->pointer_writes_ignored && (addr == 0x04 || addr == 0x06)) {
                    continue;  // silicon: pointers only move via FIFO_DATA reads
                }
                dev->regs[addr] = tx[i];
                if (addr == 0x04 || addr == 0x06) {
                    // Re-pointing makes the old contents unreachable; on
                    // silicon they are gone (slots refill from WR).
                    dev->fifo_len = 0;
                }
            }
            // A MODE_CONFIG RESET write latches the pending chip reset; it is
            // applied when the driver polls the self-clearing bit low. A
            // reset-ignoring clone drops the write: the register keeps its
            // old value, so the poll reads "already clear" and only a
            // readback of the RESET-CLEARED registers exposes the lie.
            if (tx_len >= 2 && start == FakeRegisterDevice::kModeConfigReg &&
                (tx[1] & FakeRegisterDevice::kModeResetBit) != 0 && !dev->reset_ignored) {
                dev->reset_pending = true;
                dev->reset_polls_left = dev->reset_hold_polls;
                dev->regs[FakeRegisterDevice::kModeConfigReg] = FakeRegisterDevice::kModeResetBit;
            }
        }
        // Read phase: sequential read from the register after the last written
        // tx byte — for a pure read tx is just the register address.
        if (rx_len > 0 && rx != nullptr) {
            if (tx_len >= 1 && tx[0] == kFifoDataReg && rx_len > 1) {
                readFifo(*dev, rx, rx_len);
            } else if (tx_len >= 1 && tx[0] == FakeRegisterDevice::kModeConfigReg && rx_len == 1 &&
                       dev->reset_pending) {
                // Poll of the self-clearing reset bit: `reset_hold_polls`
                // reads report "in progress", then the reset applies and the
                // bit reads 0.
                if (dev->reset_polls_left > 0) {
                    --dev->reset_polls_left;
                    rx[0] = FakeRegisterDevice::kModeResetBit;
                } else {
                    dev->applyPowerOnDefaults();
                    rx[0] = dev->regs[FakeRegisterDevice::kModeConfigReg];
                }
            } else {
                const uint8_t start = (tx_len >= 1) ? tx[0] : 0;
                for (size_t i = 0; i < rx_len; ++i) {
                    rx[i] = dev->regs[static_cast<uint8_t>(start + i)];
                }
            }
        }
        return bw::Err::Ok;
    }

    // [HW] MAX30102 FIFO_DATA register (same value as in max30102_ppg.cpp).
    static constexpr uint8_t kFifoDataReg = 0x07;

private:
    FakeRegisterDevice* find(uint8_t addr7) {
        for (size_t i = 0; i < device_count_; ++i) {
            if (addrs_[i] == addr7) {
                return &devices_[i];
            }
        }
        return nullptr;
    }

    // FIFO read: pop rx_len bytes (whole 6-byte samples), advance the model's
    // 5-bit read pointer the way the silicon does — unless the pop is broken
    // (bytes still come out, but RD never moves: no software drain path).
    static void readFifo(FakeRegisterDevice& dev, uint8_t* rx, size_t rx_len) {
        const size_t n = (rx_len < dev.fifo_len) ? rx_len : dev.fifo_len;
        std::memcpy(rx, dev.fifo, n);
        if (n < rx_len) {
            std::memset(rx + n, 0, rx_len - n);  // reading past seeded data
        }
        std::memmove(dev.fifo, dev.fifo + n, dev.fifo_len - n);
        dev.fifo_len -= n;
        if (!dev.fifo_pop_broken) {
            const size_t samples = n / 6;
            dev.regs[0x06] = static_cast<uint8_t>((dev.regs[0x06] + samples) & 0x1F);
        }
    }

    FakeRegisterDevice devices_[kMaxDevices] = {};
    uint8_t addrs_[kMaxDevices] = {};
    size_t device_count_ = 0;

    Transfer transfers_[kMaxTransfers] = {};
    size_t transfer_count_ = 0;
    size_t recover_count_ = 0;
};

}  // namespace bw::hal::test
