#pragma once

#include <bw/testkit/csv_row.h>
#include <bw/testkit/host_only.h>

#include <bw/hardware.h>
#include <bw/result.h>
#include <bw/types.h>

namespace bw::testkit {

// [EXPERIMENTAL] Script capacity in samples. Why 1024: 1024 IMU samples are 20+ s
// of 50 Hz replay (01 §3.3) — enough for windowed-logic tests without dynamic
// allocation; longer replays instantiate FakeImu<N> with a bigger N.
constexpr size_t kDefaultImuScriptSamples = 1024;

// FakeImu — bw::IImuSensor fake (02 §3) with a scripted sample queue and whole-file
// CSV replay (02 §6.4 capture rows). Deterministic: the same fixture text replays
// the same timestamps and sample count on every run (F00 §9's contract for
// test_csv_row.cpp).
//
// Lifecycle convenience: begin()/reinit() set status to Ok on success and Failed
// on failure (the usual HAL contract); setStatus() overrides at any time. read()
// deliberately does NOT consult status_ — coupling reads to status is F01 policy,
// not the fake's. read() is non-blocking and returns Ok,0 when the script is
// drained (02 §3).
template <size_t Capacity = kDefaultImuScriptSamples> class FakeImu final : public bw::IImuSensor {
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

    bw::Result<size_t> read(bw::ImuSample* out, size_t max_samples) override {
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
    bw::Result<void> enqueue(const bw::ImuSample& sample) {
        if (count_ >= Capacity) {
            return {bw::Err::Full};
        }
        script_[(head_ + count_) % Capacity] = sample;
        ++count_;
        return {bw::Err::Ok};
    }

    // Synthetic still-wrist stream: accel = {0, 0, 1 g} (gravity on z), gyro = 0,
    // stamped start_ms + i*period_ms. Why a helper: windowed tests need hundreds
    // of samples without hand-writing a fixture; tests that need motion build
    // ImuSample values and call enqueue() in a loop.
    bw::Result<void> enqueueSynthetic(size_t count, bw::TimeMs start_ms, bw::TimeMs period_ms) {
        if (count > Capacity - count_) {
            return {bw::Err::Full};  // CR-004: refused, nothing written
        }
        for (size_t i = 0; i < count; ++i) {
            bw::ImuSample sample{};
            sample.t_ms = start_ms + i * period_ms;
            sample.accel_g[0] = 0.0f;
            sample.accel_g[1] = 0.0f;
            sample.accel_g[2] = 1.0f;
            sample.gyro_dps[0] = 0.0f;
            sample.gyro_dps[1] = 0.0f;
            sample.gyro_dps[2] = 0.0f;
            const bw::Result<void> queued = enqueue(sample);
            (void)queued;  // capacity pre-checked above; cannot fail here
        }
        return {bw::Err::Ok};
    }

    // Parses a whole capture text (header + "I,t,ax,ay,az,gx,gy,gz" rows, 02 §6.4)
    // and enqueues every row. All-or-nothing: a bad file returns the first row's
    // error and leaves the script exactly as it was (two passes — validation first),
    // so a failing test never runs on half-loaded state. Blank lines and the header
    // are skipped; a script that would exceed Capacity returns Err::Full.
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
                if (isImuCsvHeader(line, size)) {
                    continue;
                }
                const bw::Result<bw::ImuSample> parsed = parseImuCsvRow(line, size);
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
            if (size == 0 || isImuCsvHeader(line, size)) {
                continue;
            }
            const bw::Result<bw::ImuSample> parsed = parseImuCsvRow(line, size);
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

    // Forces the next begin()/reinit()/read() result (fault-injection hook;
    // Err::Ok = normal behaviour). begin()/reinit() still update status_ from it.
    void setBeginResult(bw::Err err) { begin_err_ = err; }
    void setReinitResult(bw::Err err) { reinit_err_ = err; }
    void setReadResult(bw::Err err) { read_err_ = err; }
    void setStatus(bw::SensorStatus status) { status_ = status; }

    // Back to a fresh fake: script drained, counters zeroed, forced results
    // cleared, status Uninitialized.
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
    bw::ImuSample script_[Capacity];
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
