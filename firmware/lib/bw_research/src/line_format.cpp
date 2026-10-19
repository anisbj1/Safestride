#include <bw/research/line_format.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <bw/research/research_config.h>

namespace bw::research {
namespace {

// Fixed formats (D9, DATASET_FORMAT.md "Field formats"): integers plain, g
// values %.4f, angles/dps/bpm %.1f, confidences/SQI %.3f, perfusion %.5f,
// frequency %.3f. Rationale: ≥4 significant digits keeps the print rounding
// error below any pipeline threshold, so a replay can reproduce features from
// the rows within kFeatureTolerance* (tools/replay).

// Scratch: every line is built here first, so a too-small cap leaves `out`
// untouched and no partial line is ever emitted (F28 §10 contract).
constexpr size_t kScratchBytes = kMaxLineLen + 1;

// Builds one line with printf semantics; sets overflow once the line no longer
// fits kScratchBytes (then finalize() drops the row and the caller counts it).
class LineBuilder {
public:
    void append(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vappend(fmt, args);
        va_end(args);
    }

    void vappend(const char* fmt, va_list args) {
        if (overflow_) {
            return;
        }
        const size_t remaining = kScratchBytes - off_;
        const int written = std::vsnprintf(scratch_ + off_, remaining, fmt, args);
        if (written < 0 || static_cast<size_t>(written) >= remaining) {
            overflow_ = true;  // would not fit: discard, never truncate mid-row
            return;
        }
        off_ += static_cast<size_t>(written);
    }

    // Terminates with '\n' (if not already present) and copies into out.
    // Returns the line length including '\n', or 0 if it does not fit `cap`.
    size_t finish(char* out, size_t cap) {
        if (overflow_) {
            return 0;
        }
        if (off_ == 0 || scratch_[off_ - 1] != '\n') {
            if (off_ + 1 >= kScratchBytes) {
                return 0;
            }
            scratch_[off_++] = '\n';
        }
        if (off_ + 1 > cap) {  // needs room for the line AND its NUL
            return 0;
        }
        std::memcpy(out, scratch_, off_ + 1);
        return off_;
    }

private:
    char scratch_[kScratchBytes]{};
    size_t off_ = 0;
    bool overflow_ = false;
};

// Enum spellings are part of the format contract (DATASET_FORMAT.md); they are
// printed by NAME so a row stays readable if a numeric value is ever appended
// to (append-only enums, 02 §6.6).
const char* qualityName(bw::DataQuality quality) {
    switch (quality) {
    case bw::DataQuality::Good:
        return "Good";
    case bw::DataQuality::Degraded:
        return "Degraded";
    case bw::DataQuality::Insufficient:
        return "Insufficient";
    }
    return "Unknown";  // never happens for uint8_t-backed enums; keeps the row well-formed
}

const char* activityName(bw::ActivityContext context) {
    switch (context) {
    case bw::ActivityContext::Unknown:
        return "Unknown";
    case bw::ActivityContext::Stationary:
        return "Stationary";
    case bw::ActivityContext::Sitting:
        return "Sitting";
    case bw::ActivityContext::Standing:
        return "Standing";
    case bw::ActivityContext::Walking:
        return "Walking";
    case bw::ActivityContext::Running:
        return "Running";
    case bw::ActivityContext::LyingResting:
        return "LyingResting";
    case bw::ActivityContext::InactiveProlonged:
        return "InactiveProlonged";
    }
    return "Unknown";
}

const char* eventTypeName(bw::EventType type) {
    switch (type) {
    case bw::EventType::SleepEpisode:
        return "SleepEpisode";
    case bw::EventType::DrowsinessEvent:
        return "DrowsinessEvent";
    case bw::EventType::SuspectedCataplexy:
        return "SuspectedCataplexy";
    case bw::EventType::UserMarked:
        return "UserMarked";
    case bw::EventType::AlertRaised:
        return "AlertRaised";
    case bw::EventType::SensorFault:
        return "SensorFault";
    case bw::EventType::DeviceNotWorn:
        return "DeviceNotWorn";
    case bw::EventType::BatteryWarning:
        return "BatteryWarning";
    case bw::EventType::NapPlanned:
        return "NapPlanned";
    case bw::EventType::NapCompleted:
        return "NapCompleted";
    case bw::EventType::TimeSync:
        return "TimeSync";
    case bw::EventType::Boot:
        return "Boot";
    }
    return "Unknown";
}

const char* userLabelName(bw::UserLabel label) {
    switch (label) {
    case bw::UserLabel::Unlabeled:
        return "Unlabeled";
    case bw::UserLabel::Confirmed:
        return "Confirmed";
    case bw::UserLabel::Rejected:
        return "Rejected";
    }
    return "Unlabeled";
}

const char* alertnessName(bw::AlertnessState state) {
    switch (state) {
    case bw::AlertnessState::Unknown:
        return "Unknown";
    case bw::AlertnessState::NotWorn:
        return "NotWorn";
    case bw::AlertnessState::Awake:
        return "Awake";
    case bw::AlertnessState::Drowsy:
        return "Drowsy";
    case bw::AlertnessState::HighDrowsinessRisk:
        return "HighDrowsinessRisk";
    case bw::AlertnessState::PossibleSleep:
        return "PossibleSleep";
    }
    return "Unknown";
}

// Copies the firmware version into a fixed buffer, replacing ',' and any
// newline with '_' (D6: the parser counts fields by ',' — the row shape must
// stay stable no matter what the version string contains) and truncating to
// kMaxFirmwareVersionLen.
void sanitiseVersion(const char* version, char* out, size_t cap) {
    if (version == nullptr) {
        std::snprintf(out, cap, "unknown");
        return;
    }
    size_t i = 0;
    for (; i + 1 < cap && version[i] != '\0'; ++i) {
        const char c = version[i];
        out[i] = (c == ',' || c == '\n' || c == '\r') ? '_' : c;
    }
    out[i] = '\0';
}

}  // namespace

size_t formatHeader(char* out, size_t cap, const HeaderInfo& info) {
    char version[kMaxFirmwareVersionLen + 1];
    sanitiseVersion(info.firmware_version, version, sizeof(version));

    LineBuilder line;
    line.append("#HDR,%u,%u,%s,%u,%u,%u,%u", info.schema_version, info.boot_id, version,
                info.imu_drops, info.ppg_drops, info.line_drops, info.feature_missed);
    return line.finish(out, cap);
}

size_t formatImu(char* out, size_t cap, const bw::ImuSample& sample) {
    LineBuilder line;
    line.append("I,%llu,%.4f,%.4f,%.4f,%.1f,%.1f,%.1f",
                static_cast<unsigned long long>(sample.t_ms),
                static_cast<double>(sample.accel_g[0]), static_cast<double>(sample.accel_g[1]),
                static_cast<double>(sample.accel_g[2]), static_cast<double>(sample.gyro_dps[0]),
                static_cast<double>(sample.gyro_dps[1]), static_cast<double>(sample.gyro_dps[2]));
    return line.finish(out, cap);
}

size_t formatPpg(char* out, size_t cap, const bw::PpgSample& sample) {
    LineBuilder line;
    line.append("P,%llu,%u,%u", static_cast<unsigned long long>(sample.t_ms), sample.red,
                sample.ir);
    return line.finish(out, cap);
}

size_t formatMotionFeatures(char* out, size_t cap, const bw::MotionFeatures& features) {
    LineBuilder line;
    line.append("F,M,%llu,%s,%.4f,%.4f,%.4f,%.1f,%.1f,%.3f,%u",
                static_cast<unsigned long long>(features.t_ms), qualityName(features.quality),
                static_cast<double>(features.enmo_g), static_cast<double>(features.mag_std_g),
                static_cast<double>(features.jerk_rms_g_per_s),
                static_cast<double>(features.gyro_rms_dps),
                static_cast<double>(features.gravity_tilt_deg),
                static_cast<double>(features.dominant_freq_hz), features.stillness_ms);
    return line.finish(out, cap);
}

size_t formatPpgFeatures(char* out, size_t cap, const bw::PpgFeatures& features) {
    LineBuilder line;
    line.append("F,P,%llu,%s,%d,%.1f,%.3f,%.5f,%.1f,%.3f,%d,%d,%.1f",
                static_cast<unsigned long long>(features.t_ms), qualityName(features.quality),
                features.hr_valid ? 1 : 0, static_cast<double>(features.hr_bpm),
                static_cast<double>(features.hr_confidence),
                static_cast<double>(features.perfusion_index),
                static_cast<double>(features.ir_dc_level), static_cast<double>(features.sqi),
                features.motion_corrupted ? 1 : 0, features.hrv_valid ? 1 : 0,
                static_cast<double>(features.rmssd_ms));
    return line.finish(out, cap);
}

size_t formatContext(char* out, size_t cap, const bw::ActivityEstimate& estimate) {
    LineBuilder line;
    line.append("F,C,%llu,%s,%.3f", static_cast<unsigned long long>(estimate.t_ms),
                activityName(estimate.context), static_cast<double>(estimate.confidence));
    return line.finish(out, cap);
}

size_t formatEvent(char* out, size_t cap, const bw::Event& event) {
    char payload_hex[bw::Event::kMaxPayloadBytes * 2 + 1];
    size_t hex_off = 0;
    for (size_t i = 0; i < bw::Event::kMaxPayloadBytes; ++i) {
        std::snprintf(payload_hex + hex_off, sizeof(payload_hex) - hex_off, "%02x",
                      event.payload[i]);
        hex_off += 2;
    }

    LineBuilder line;
    line.append("E,%u,%llu,%u,%u,%u,%.3f,%u,%s,%s,%s,%s,%s,%s,%s,%u,%u", event.seq,
                static_cast<unsigned long long>(event.t_start_ms), event.boot_id, event.duration_ms,
                event.t_start_unix, static_cast<double>(event.confidence), event.reasons,
                payload_hex, qualityName(event.quality.imu), qualityName(event.quality.ppg),
                eventTypeName(event.type), userLabelName(event.label), activityName(event.context),
                alertnessName(event.alertness_at_start), static_cast<unsigned>(event.detector_id),
                static_cast<unsigned>(event.schema_version));
    return line.finish(out, cap);
}

size_t formatMarker(char* out, size_t cap, bw::TimeMs t_ms, uint32_t event_seq) {
    LineBuilder line;
    line.append("M,%llu,user_marked#%u", static_cast<unsigned long long>(t_ms), event_seq);
    return line.finish(out, cap);
}

}  // namespace bw::research
