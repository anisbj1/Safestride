#pragma once

#include <bw/testkit/csv_row.h>
#include <bw/testkit/host_only.h>

#include <bw/hardware.h>
#include <bw/result.h>
#include <bw/types.h>

namespace bw::testkit {

// [EXPERIMENTAL] Script capacity in samples. Why 1024: the PPG FIFO is drained at
// ≤100 ms intervals (01 §3.2), so 1024 samples span 100+ s of wall time at 100 Hz
// (01 §3.3) — several minutes-worth for windowed tests; larger replays instantiate
// FakePpg<N>.
constexpr size_t kDefaultPpgScriptSamples = 1024;

// FakePpg — bw::IPpgSensor fake (02 §3): scripted sample queue plus whole-file CSV
// replay of the 02 §6.4 capture rows ("P,t,red,ir"). Same determinism contract as
// FakeImu: the same fixture text replays the same timestamps and count on every
// run. read() is non-blocking and returns Ok,0 when drained (02 §3).
//
// Lifecycle convenience and the status_/read() separation are identical to
// FakeImu — see fake_imu.h for the rationale (F01 owns the real coupling).
template <size_t Capacity = kDefaultPpgScriptSamples> class FakePpg final : public bw::IPpgSensor {
public:
    bw::Result<void> begin() override {
        ++begin_calls_;
        if (begin_err_ != bw::Err::Ok) {
            status_ = bw::SensorStatus::Failed;
            return {begin_err_};
        }
        status_ = bw::SensorStatus::Ok;
        return {bw::Err::Ok};
    }

    bw::Result<size_t> read(bw::PpgSample* out, size_t max_samples) override {
        ++read_calls_;
        if (read_err_ != bw::Err::Ok) {
            return {read_err_, 0};
        }
        if (max_samples > 0 && out == nullptr) {
            return {bw::Err::InvalidArg, 0};
        }
        size_t written = 0;
        while (written < max_samples && count_ > 0) {
            out[written] = script_[head_];
            head_ = (head_ + 1) % Capacity;
            --count_;
            ++delivered_;
            ++written;
        }
        return {bw::Err::Ok, written};
    }

    bw::SensorStatus status() const override { return status_; }

    bw::Result<void> reinit() override {
        ++reinit_calls_;
        if (reinit_err_ != bw::Err::Ok) {
            status_ = bw::SensorStatus::Failed;
            return {reinit_err_};
        }
        status_ = bw::SensorStatus::Ok;
        return {bw::Err::Ok};
    }

    // ---- scripting ------------------------------------------------------------

    // Queues one sample. Err::Full when the script is at Capacity (CR-004:
    // refused, nothing written).
    bw::Result<void> enqueue(const bw::PpgSample& sample) {
        if (count_ >= Capacity) {
            return {bw::Err::Full};
        }
        script_[(head_ + count_) % Capacity] = sample;
        ++count_;
        return {bw::Err::Ok};
    }

    // Synthetic constant stream (flatline/DC-level fixtures), stamped
    // start_ms + i*period_ms, unsaturated. Tests that need a pulse waveform build
    // PpgSample values and call enqueue() in a loop.
    bw::Result<void> enqueueSynthetic(size_t count, bw::TimeMs start_ms, bw::TimeMs period_ms,
                                      uint32_t red, uint32_t ir) {
        if (count > Capacity - count_) {
            return {bw::Err::Full};  // CR-004: refused, nothing written
        }
        for (size_t i = 0; i < count; ++i) {
            bw::PpgSample sample{};
            sample.t_ms = start_ms + i * period_ms;
            sample.red = red;
            sample.ir = ir;
            sample.saturated = false;
            const bw::Result<void> queued = enqueue(sample);
            (void)queued;  // capacity pre-checked above; cannot fail here
        }
        return {bw::Err::Ok};
    }

    // Parses a whole capture text (header + "P,t,red,ir" rows, 02 §6.4) and
    // enqueues every row. All-or-nothing, blank lines and header skipped, Err::Full
    // on capacity — exactly FakeImu::loadCsv's semantics (see fake_imu.h).
    bw::Result<size_t> loadCsv(const char* text) {
        if (text == nullptr) {
            return {bw::Err::InvalidArg, 0};
        }

        // Pass 1: validate without touching the script.
        size_t rows = 0;
        {
            CsvLineReader reader(text);
            const char* line = nullptr;
            size_t size = 0;
            while (reader.next(line, size)) {
                if (size == 0) {
                    continue;  // blank line (including a trailing newline)
                }
                if (size > kMaxCsvLineBytes) {
                    return {bw::Err::Overflow, 0};
                }
                if (isPpgCsvHeader(line, size)) {
                    continue;
                }
                const bw::Result<bw::PpgSample> parsed = parsePpgCsvRow(line, size);
                if (!parsed.ok()) {
                    return {parsed.err, 0};
                }
                ++rows;
            }
        }
        if (rows > Capacity - count_) {
            return {bw::Err::Full, 0};  // CR-004: refused, nothing written
        }

        // Pass 2: enqueue (re-parses; cheap on the host, keeps pass 1 side-effect free).
        CsvLineReader reader(text);
        const char* line = nullptr;
        size_t size = 0;
        while (reader.next(line, size)) {
            if (size == 0 || isPpgCsvHeader(line, size)) {
                continue;
            }
            const bw::Result<bw::PpgSample> parsed = parsePpgCsvRow(line, size);
            const bw::Result<void> queued = enqueue(parsed.value);
            (void)queued;  // pass 1 validated the row count against Capacity
        }
        return {bw::Err::Ok, rows};
    }

    // ---- observation & knobs --------------------------------------------------

    size_t remaining() const { return count_; }
    size_t delivered() const { return delivered_; }  // samples handed out by read()
    size_t beginCalls() const { return begin_calls_; }
    size_t reinitCalls() const { return reinit_calls_; }

    // Fault-injection hooks; Err::Ok = normal behaviour (see fake_imu.h).
    void setBeginResult(bw::Err err) { begin_err_ = err; }
    void setReinitResult(bw::Err err) { reinit_err_ = err; }
    void setReadResult(bw::Err err) { read_err_ = err; }
    void setStatus(bw::SensorStatus status) { status_ = status; }

    // Back to a fresh fake (semantics identical to FakeImu::reset).
    void reset() {
        head_ = 0;
        count_ = 0;
        delivered_ = 0;
        begin_calls_ = 0;
        reinit_calls_ = 0;
        read_calls_ = 0;
        begin_err_ = bw::Err::Ok;
        reinit_err_ = bw::Err::Ok;
        read_err_ = bw::Err::Ok;
        status_ = bw::SensorStatus::Uninitialized;
    }

private:
    bw::PpgSample script_[Capacity];
    size_t head_ = 0;
    size_t count_ = 0;
    size_t delivered_ = 0;
    size_t begin_calls_ = 0;
    size_t reinit_calls_ = 0;
    size_t read_calls_ = 0;
    bw::Err begin_err_ = bw::Err::Ok;
    bw::Err reinit_err_ = bw::Err::Ok;
    bw::Err read_err_ = bw::Err::Ok;
    bw::SensorStatus status_ = bw::SensorStatus::Uninitialized;
};

}  // namespace bw::testkit
