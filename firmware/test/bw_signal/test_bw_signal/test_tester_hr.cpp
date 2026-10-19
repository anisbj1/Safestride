#include <bw/signal/signal_processor.h>
#include <bw/signal/tester_heart_rate.h>
#include <unity.h>

namespace {
using namespace bw::signal;
void sample(TesterHeartRate& hr, uint32_t ir, bw::TimeMs time) {
    (void)hr.push(bw::PpgSample{time, ir, ir, false});
}
void peak(TesterHeartRate& hr, bw::TimeMs time) {
    sample(hr, 60000, time - 80);
    sample(hr, 64000, time - 40);
    sample(hr, 60000, time);
}
// Measured cycle from the independent tester's test_heart_rate.py, 25 Hz FIFO.
constexpr uint32_t kWave[] = {151395, 150784, 149849, 149114, 148842, 148787, 148820,
                              149027, 149244, 149330, 149319, 149314, 149398, 149542,
                              149691, 149915, 150082, 150224, 150332, 150441, 150540,
                              150621, 150725, 150854, 150970, 151090};
}  // namespace

void test_tester_hr_placement_average_expiry_and_recontact() {
    TesterHeartRate hr;
    sample(hr, 60000, 100);
    peak(hr, 1000);
    TEST_ASSERT_FALSE(hr.valid());
    peak(hr, 2000);
    TEST_ASSERT_TRUE(hr.valid());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, hr.bpm());
    peak(hr, 2800);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 67.5f, hr.bpm());
    hr.updateTime(5800);
    TEST_ASSERT_TRUE(hr.valid());
    hr.updateTime(5801);
    TEST_ASSERT_FALSE(hr.valid());
    peak(hr, 6500);
    TEST_ASSERT_FALSE(hr.valid());
    peak(hr, 7300);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 75.0f, hr.bpm());
    sample(hr, 1000, 7400);
    TEST_ASSERT_FALSE(hr.contact());
    TEST_ASSERT_FALSE(hr.valid());
    sample(hr, 60000, 7500);
    peak(hr, 8000);
    TEST_ASSERT_FALSE(hr.valid());
    peak(hr, 9000);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, hr.bpm());
}

void test_tester_hr_captured_waveform_and_noise() {
    TesterHeartRate hr;
    bw::TimeMs time = 100;
    for (int cycle = 0; cycle < 8; ++cycle) {
        for (uint32_t ir : kWave) {
            sample(hr, ir, time);
            time += 40;
        }
    }
    TEST_ASSERT_TRUE(hr.valid());
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 60000.0f / 1040.0f, hr.bpm());
    TesterHeartRate noise;
    for (bw::TimeMs t = 100; t < 10100; t += 40) {
        sample(noise, 150000 + ((t / 40) % 2) * 30, t);
    }
    TEST_ASSERT_FALSE(noise.valid());
}

void test_tester_profile_ambient_noise_is_insufficient_without_contact() {
    SignalProcessor processor{PpgProfile::Tester25Hz};
    SignalWindowOutput output{};
    for (bw::TimeMs time = 0; time < 20000; time += 40) {
        const uint32_t ir = 450 + ((time / 40) % 2) * 120;
        const bw::PpgSample point{time, ir, ir, false};
        processor.feedPpg(&point, 1);
        if (processor.stepUntilIdle(time, output)) {
            TEST_ASSERT_FALSE(output.ppg.hr_valid);
            TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::DataQuality::Insufficient),
                                  static_cast<int>(output.ppg.quality));
            TEST_ASSERT_EQUAL_FLOAT(0.0f, output.ppg.sqi);
            TEST_ASSERT_EQUAL_FLOAT(0.0f, output.ppg.hr_bpm);
        }
    }
}

void test_tester_hr_four_value_rollover_and_saturation() {
    TesterHeartRate hr;
    // Beyond 32-bit millis: timestamps and subtraction must not wrap.
    constexpr bw::TimeMs kStart = (bw::TimeMs{1} << 32);
    sample(hr, 60000, kStart + 100);
    peak(hr, kStart + 1000);
    peak(hr, kStart + 2000);  // 60
    peak(hr, kStart + 2800);  // 75
    peak(hr, kStart + 4000);  // 50
    peak(hr, kStart + 5000);  // 60
    peak(hr, kStart + 5600);  // 100; replaces the oldest 60
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 71.25f, hr.bpm());
    const bw::PpgSample saturated{kStart + 5640, 262143, 262143, true};
    TEST_ASSERT_FALSE(hr.push(saturated));
    TEST_ASSERT_FALSE(hr.valid());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, hr.bpm());
}

void test_tester_profile_windowed_pipeline_captured_waveform() {
    SignalProcessor processor{PpgProfile::Tester25Hz};
    SignalWindowOutput output{};
    bool found = false;
    for (bw::TimeMs time = 0; time < 12000; time += 40) {
        const uint32_t ir = kWave[(time / 40) % 26];
        const bw::PpgSample point{time, ir, ir, false};
        processor.feedPpg(&point, 1);
        if (processor.stepUntilIdle(time, output) && output.ppg.hr_valid) {
            found = true;
            TEST_ASSERT_FLOAT_WITHIN(1.0f, 60000.0f / 1040.0f, output.ppg.hr_bpm);
            TEST_ASSERT_TRUE(output.ppg_sample_count >= 24 && output.ppg_sample_count <= 26);
        }
    }
    TEST_ASSERT_TRUE(found);
    TEST_ASSERT_TRUE(output.ppg.quality != bw::DataQuality::Insufficient);
    // Silence expires both the published validity and the detector average.
    TEST_ASSERT_TRUE(processor.stepUntilIdle(16000, output));
    TEST_ASSERT_FALSE(output.ppg.hr_valid);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, output.ppg.hr_bpm);
}
