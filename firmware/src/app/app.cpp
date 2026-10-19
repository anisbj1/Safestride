#include "app.h"

#include <Arduino.h>

#include <cstdio>  // snprintf for the bench address-list formatter

// bw/core.h is the umbrella (F00 D4); including it here means the DEVICE build
// also compiles bw/contract_checks.h (D3): layout drift in any contract type
// fails `pio run -e esp32dev`, not only the host tests.
#include <bw/core.h>

#include "wiring/task_health.h"
#include "wiring/wire_activity.h"
#include "wiring/wire_alert.h"
#include "wiring/wire_baseline.h"  // W6 P4: F13 personalized baseline
#include "wiring/wire_ble.h"       // W4 P4 (2026-10-16): F19 BLE GATT server (comms slot)
#include "wiring/wire_cataplexy.h"
#include "wiring/wire_confirm.h"
#include "wiring/wire_diag.h"
#include "wiring/wire_modes.h"  // W6 P4: F24 operating modes
// W3 P4 (2026-10-13): events & storage — the logger's bench [EVT] line uses
// F22's reason text, so the explain seam ships in the same dataflow.
#include "wiring/wire_eventlog.h"
#include "wiring/wire_explain.h"
#include "wiring/wire_f02.h"
#include "wiring/wire_hal.h"
#include "wiring/wire_history.h"  // W4 P4: F16 history aggregation (services slot)
#include "wiring/wire_input.h"
#include "wiring/wire_ml.h"
#include "wiring/wire_reliability.h"  // W6 P4: F26 fault handling & reliability
#include "wiring/wire_research.h"
#include "wiring/wire_sleep.h"
#include "wiring/wire_state.h"
#include "wiring/wire_storage.h"
#include "wiring/wire_sync.h"  // W4 P4: F20 sync engine (comms slot, over F19's transport)
#include "wiring/wire_ui.h"
#include "wiring/wire_wake.h"
#include "wiring/wire_wear.h"

namespace app {

// Version marker, not a threshold/timeout/rate/ML parameter — 00 §7.5's tag system
// does not apply (F00 D9). 02 §6.2 transmits this value in the BLE Device Info
// characteristic once F19 exists; bump it whenever a release leaves the bench.
constexpr const char* kFirmwareVersion = "0.1.0";

// W4 P4 (2026-10-16): BLE Device Information strings the app layer owns
// (F19 D4, spec §18). hw_rev names the board from materiel.txt (owner-
// confirmed 2026-09-29); like the version marker above it is a build fact,
// not a threshold/timeout/rate — 00 §7.5's tag system does not apply.
constexpr const char* kHwRevision = "esp32-wroom-dev-breadboard";

// [DERIVED] Matches monitor_speed = 115200 in platformio.ini, so the banner is
// readable by `pio device monitor` with no extra flags. Why here at all: Serial
// must be started once before the first print (DoD §14: the esp32dev firmware
// prints a version banner).
constexpr uint32_t kSerialBaud = 115200;

namespace {

// --- F01 Bench A scaffolding (M1 bring-up) ----------------------------------
// The 01 §3.2 tasks landed 2026-10-08 (created inside each wire(), see run());
// this loop keeps the developer-facing bench only — buttons, LED mirror,
// beeper, 2 s diagnostics — and no longer reads sensors: SensorTask owns the
// I²C sample path, and the mutex-free [RAW] probes went with it. Full bench
// removal stays on F01's carried list. Scaffolding still obeys 00 §7.5:
// every value is a named constant with one tag and a why.

// [EXPERIMENTAL] Bench A rows ④/⑤ (2026-10-02, developer's test design): the
// loop no longer cycles the RGB automatically — the LED MIRRORS the buttons:
// idle = off, BIG pressed = red, SMALL pressed = green, both = blue. A stuck
// input therefore shows as a lit LED with nobody touching the board. The pin
// finder from 2026-10-01 stays available for a future wrong-pin hunt.
constexpr bool kPinFinderEnabled = false;

// [EXPERIMENTAL] Diagnostic cadence: ~300 B per line at 115200 keeps the
// scroll readable, and 2 s is short enough that a bus/sensor state change is
// never silent for long. Tune at the bench; coexists with SensorTask's silent
// 20 Hz sampling since 2026-10-08 (the loop no longer reads sensors itself).
constexpr uint32_t kDiagPeriodMs = 2000;

// [EXPERIMENTAL] Beep every 5 s: slow enough to count by ear, and still an
// independent "alive" heartbeat now that the LED is button-driven, so a stalled
// loop can never hide behind a frozen observable.
constexpr uint32_t kBuzzerBeepPeriodMs = 5000;

// [DERIVED] 60 ms reads as a discrete "blip" (a click/short tone is perceived
// as an event at ~50-100 ms) and keeps the duty cycle at ~1 % — the bench must
// never become a continuous tone.
constexpr uint32_t kBuzzerBeepMs = 60;

// [HW] 2 kHz requested for the bench tone; the same value as
// kBuzzDefaultFreqHz (F01 spec §8): mid-audible, where a typical passive
// piezo is loudest (A-02 passive confirmed).
constexpr uint16_t kBuzzerBeepFreqHz = 2000;

// [USER] Presentation build (2026-10-18, developer request): the 5 s liveness
// heartbeat above is MUTED so the buzzer speaks only for alerts during the
// demo/video. Set true to restore the bench heartbeat; host suites are
// unaffected (this loop is device-only). Alert patterns (F10) are unchanged.
constexpr bool kBuzzerHeartbeatEnabled = false;

// [DERIVED] 10 ms tick = 100 Hz. 100x finer than the slowest bench period
// (kDiagPeriodMs), so a ~100 ms human button press always lands on the edge
// print, while each tick costs only a few GPIO reads — no busy spin, every
// wait goes through vTaskDelay (00 §7.2: no blocking delay()).
constexpr uint32_t kLoopTickMs = 10;

// [DERIVED] Room for the three devices this board can carry (display 0x3C,
// MPU6050 0x68/0x69, MAX30102 0x57) plus headroom for a surprise answer on
// the bus — enough to prove row ② without a bigger buffer.
constexpr size_t kMaxScanAddrs = 16;

// [DERIVED] "0x3C " is 5 characters per address, + NUL for the terminator.
constexpr size_t kScanListChars = kMaxScanAddrs * 5;

// [HW] Rgb channels are uint8_t (02 §3): 255 = full-scale PWM duty, i.e.
// "LED fully on" for whichever channel the bench is testing.
constexpr uint8_t kLedOn = 255;

// [HW] The "F01 alive" canvas block used to live here (panel geometry note:
// y was the glyph BASELINE, u8g2 semantics). Removed at W2 P4 (2026-10-12):
// F12's UiTask owns the panel now, so the bench draws only once at boot
// (one-shot probe in run()) and never again — two writers = visible flicker.

// Row ⑤ (A-08): full-scale on one channel at a time, so a wrong leg or a
// reversed polarity shows up as a wrong colour, a dim colour, or nothing.
constexpr bw::Rgb kBenchColours[] = {
    {kLedOn, 0, 0},
    {0, kLedOn, 0},
    {0, 0, kLedOn},
};
constexpr size_t kBenchColourCount = sizeof(kBenchColours) / sizeof(kBenchColours[0]);

const char* colourName(size_t idx) {
    switch (idx % kBenchColourCount) {
    case 0:
        return "RED";
    case 1:
        return "GREEN";
    default:
        return "BLUE";
    }
}

const char* errName(bw::Err e) {
    switch (e) {
    case bw::Err::Ok:
        return "Ok";
    case bw::Err::Timeout:
        return "Timeout";
    case bw::Err::BusError:
        return "BusError";
    case bw::Err::NotFound:
        return "NotFound";
    case bw::Err::InvalidArg:
        return "InvalidArg";
    case bw::Err::NotReady:
        return "NotReady";
    case bw::Err::Overflow:
        return "Overflow";
    case bw::Err::Corrupt:
        return "Corrupt";
    case bw::Err::Unsupported:
        return "Unsupported";
    case bw::Err::Busy:
        return "Busy";
    case bw::Err::HwFault:
        return "HwFault";
    case bw::Err::Full:
        return "Full";
    }
    return "?";
}

const char* sensorStatusName(bw::SensorStatus s) {
    switch (s) {
    case bw::SensorStatus::Uninitialized:
        return "Uninitialized";
    case bw::SensorStatus::Ok:
        return "Ok";
    case bw::SensorStatus::Degraded:
        return "Degraded";
    case bw::SensorStatus::Failed:
        return "Failed";
    }
    return "?";
}

const char* severityName(bw::FaultSeverity s) {
    switch (s) {
    case bw::FaultSeverity::Info:
        return "Info";
    case bw::FaultSeverity::Warning:
        return "Warning";
    case bw::FaultSeverity::Error:
        return "Error";
    case bw::FaultSeverity::Critical:
        return "Critical";
    }
    return "?";
}

const char* faultCodeName(bw::FaultCode c) {
    switch (c) {
    case bw::FaultCode::None:
        return "None";
    case bw::FaultCode::I2cTimeout:
        return "I2cTimeout";
    case bw::FaultCode::I2cBusStuck:
        return "I2cBusStuck";
    case bw::FaultCode::ImuAbsent:
        return "ImuAbsent";
    case bw::FaultCode::ImuReadFailed:
        return "ImuReadFailed";
    case bw::FaultCode::PpgAbsent:
        return "PpgAbsent";
    case bw::FaultCode::PpgReadFailed:
        return "PpgReadFailed";
    case bw::FaultCode::PpgFifoOverrun:
        return "PpgFifoOverrun";
    case bw::FaultCode::DisplayFailed:
        return "DisplayFailed";
    case bw::FaultCode::BatterySenseUnavailable:
        return "BatterySenseUnavailable";
    case bw::FaultCode::ButtonStuck:
        return "ButtonStuck";
    case bw::FaultCode::ImuStuckSensor:
        return "ImuStuckSensor";
    case bw::FaultCode::PpgFlatline:
        return "PpgFlatline";
    case bw::FaultCode::PpgSaturated:
        return "PpgSaturated";
    case bw::FaultCode::PpgPoorContact:
        return "PpgPoorContact";
    case bw::FaultCode::TaskOverrun:
        return "TaskOverrun";
    case bw::FaultCode::LowHeap:
        return "LowHeap";
    case bw::FaultCode::LowBattery:
        return "LowBattery";
    case bw::FaultCode::CriticalBattery:
        return "CriticalBattery";
    case bw::FaultCode::TaskStalled:
        return "TaskStalled";
    case bw::FaultCode::WatchdogReset:
        return "WatchdogReset";
    case bw::FaultCode::Brownout:
        return "Brownout";
    case bw::FaultCode::BootLoop:
        return "BootLoop";
    case bw::FaultCode::StorageDegraded:
        return "StorageDegraded";
    case bw::FaultCode::BleDown:
        return "BleDown";
    // personalization (F13, CR-031) / system overload (F23 M5, CR-032) /
    // composition wiring (CR-033) — added with their CRs so this switch stays
    // -Wswitch-clean as the enum grows.
    case bw::FaultCode::BaselineCorrupt:
        return "BaselineCorrupt";
    case bw::FaultCode::LowStack:
        return "LowStack";
    case bw::FaultCode::TaskCreateFailed:
        return "TaskCreateFailed";
    case bw::FaultCode::EventPublishDrop:
        return "EventPublishDrop";
    }
    return "?";
}

// A5 (F28 DoD §14, 2026-10-13): while the research raw tee is active
// (capture build F28_RESEARCH_CAPTURE_ENABLED=1), every bench print in this
// file interleaves mid-row into the capture stream and tears rows — the
// 2026-10-11 fixture lost 510 rows that way. All bench prints are gated on
// this predicate; with the capture flag OFF (repo default) rawTee() is
// always null, so normal bench behavior is unchanged.
bool rawCaptureActive() {
    return f28::rawTee() != nullptr;
}

// Bench fault sink: F01's drivers report through IFaultReporter (02 §5) and
// app.h now carries the field, so every report is printed the moment it
// arrives instead of being dropped. F26 replaces this with its registry; the
// drivers and their edge-triggered rate limits (F01 D7) do not change.
class BenchFaultReporter final : public bw::IFaultReporter {
public:
    void report(bw::FaultCode code, bw::FaultSeverity severity, bw::TimeMs t_ms) override {
        ++count_;
        if (rawCaptureActive()) {
            return;  // A5: print muted during raw capture; the counter still counts
        }
        Serial.printf("[FAULT] t=%llu ms %s %s\n", static_cast<unsigned long long>(t_ms),
                      faultCodeName(code), severityName(severity));
    }

    uint32_t count() const { return count_; }

private:
    uint32_t count_ = 0;
};

BenchFaultReporter s_faults;

// "0x3C 0x68 " — always NUL-terminated, silently truncates rather than
// overflowing (no String on a hot path, 00 §7.2).
void formatAddrs(const uint8_t* addrs, size_t n, char* out, size_t out_len) {
    size_t used = 0;
    if (out_len > 0) {
        out[0] = '\0';
    }
    for (size_t i = 0; i < n; ++i) {
        const int written = snprintf(out + used, out_len - used, "%02X ", addrs[i]);
        if (written <= 0 || static_cast<size_t>(written) >= out_len - used) {
            break;  // no room for this address: stop, keep what we have
        }
        used += static_cast<size_t>(written);
    }
}

// Names for the [PIPE] line's ActivityContext (bench formatting only — the
// contract enum lives in bw/context.h and is consumed as-is elsewhere).
const char* activityContextName(bw::ActivityContext c) {
    switch (c) {
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
    return "?";
}

// W2 P4 (2026-10-12): name helpers for the [LOOP] decision-loop line —
// bench formatting only, the contract enums live in bw_core and are
// consumed as-is elsewhere.
const char* alertnessStateName(bw::AlertnessState s) {
    switch (s) {
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
    return "?";
}

const char* wearStateName(bw::WearState s) {
    switch (s) {
    case bw::WearState::Uncertain:
        return "Uncertain";
    case bw::WearState::Worn:
        return "Worn";
    case bw::WearState::NotWorn:
        return "NotWorn";
    }
    return "?";
}

const char* alertLevelName(bw::AlertLevel l) {
    switch (l) {
    case bw::AlertLevel::Normal:
        return "Normal";
    case bw::AlertLevel::Warning:
        return "Warning";
    case bw::AlertLevel::Active:
        return "Active";
    case bw::AlertLevel::Persistent:
        return "Persistent";
    }
    return "?";
}

const char* dataQualityName(bw::DataQuality q) {
    switch (q) {
    case bw::DataQuality::Good:
        return "Good";
    case bw::DataQuality::Degraded:
        return "Degraded";
    case bw::DataQuality::Insufficient:
        return "Insufficient";
    }
    return "?";
}

const char* episodePhaseName(bw::EpisodePhase p) {
    switch (p) {
    case bw::EpisodePhase::Idle:
        return "Idle";
    case bw::EpisodePhase::Candidate:
        return "Candidate";
    case bw::EpisodePhase::Active:
        return "Active";
    }
    return "?";
}

// One diagnostic cycle: I²C scan, device statuses, buttons, battery —
// Bench A rows ①-⑦ (row ③'s display probe moved to a boot-once flush in
// run(): F12 owns the panel from its wire() onward, W2 P4).
void printDiagnostics(const AppContext& ctx, const char* colour_name) {
    // A5: muted entirely during raw capture — covers every [BENCH], [IMUDIAG],
    // [PIPE], [LOOP] and [EPIS] line below (all printed in this one cycle).
    if (rawCaptureActive()) {
        return;
    }
    uint8_t addrs[kMaxScanAddrs] = {};
    size_t addr_count = 0;
    const bw::Err scan_err = f01::i2cScan(addrs, kMaxScanAddrs, &addr_count);
    char list[kScanListChars] = {};
    formatAddrs(addrs, addr_count, list, sizeof(list));

    const bw::TimeMs t_ms = (ctx.clock != nullptr) ? ctx.clock->nowMs() : 0;
    Serial.printf("[BENCH] t=%llu ms scan=%s n=%u [%s]\n", static_cast<unsigned long long>(t_ms),
                  errName(scan_err), static_cast<unsigned>(addr_count), list);
    bw::ReliabilityState reliability{};
    const bool has_reliability = f26::latestReliabilityState(reliability);
    Serial.printf(
        "[RELIABILITY] available=%u faults=%08lx degraded=%u safe=%u watchdog=%s mode_drops=%lu\n",
        has_reliability ? 1u : 0u, static_cast<unsigned long>(reliability.fault_bits),
        static_cast<unsigned>(reliability.degraded), f26::safeModeActive() ? 1u : 0u,
        errName(f26::watchdogStatus()), static_cast<unsigned long>(f10::droppedModeCommands()));

    bw::SensorStatus imu_status{}, ppg_status{};
    const bool has_status = f23::latestSensorStatuses(imu_status, ppg_status);
    Serial.printf("[BENCH] imu=%s ppg=%s btn big=%d small=%d led=%s batt=",
                  has_status ? sensorStatusName(imu_status) : "unknown",
                  has_status ? sensorStatusName(ppg_status) : "unknown",
                  f01::buttons().isPressedRaw(bw::ButtonId::Big) ? 1 : 0,
                  f01::buttons().isPressedRaw(bw::ButtonId::Small) ? 1 : 0, colour_name);

    const bw::BatteryReading batt = f01::battery().read();
    if (batt.valid) {
        Serial.print("v=");
        Serial.print(batt.voltage_v, 2);
        Serial.print("V pct=");
        Serial.print(batt.percent_estimate, 0);
        if (batt.charging_known) {
            Serial.print(batt.charging ? " charging=yes" : " charging=no");
        } else {
            Serial.print(" charging=unknown");
        }
    } else {
        // Row ⑦: valid == false (A-04, no divider) is the PASS answer.
        Serial.print("valid=0 unknown");
    }
    Serial.println();  // the line's \n used to come from the removed disp= suffix

    // W2 P4 (2026-10-12): the panel is no longer redrawn here — F12's UiTask
    // presents every frame (two writers = visible flicker on the bench).
    // The flush error stays as row ③'s probe, printed once at boot in run().

    // Bench D16: raw counts + register echo for the accel-scale anomaly —
    // printed here because Serial lives only in the composition root (01 §1);
    // the payload is F01 bench tooling (f01::imuDiag) and disappears with
    // the scaffolding it belongs to.
    char imu_diag[f01::kImuDiagMsgChars] = {};
    const bw::Err diag_err = f01::imuDiag(imu_diag, sizeof(imu_diag));
    if (diag_err == bw::Err::Ok) {
        Serial.printf("[IMUDIAG] %s\n", imu_diag);
    } else {
        Serial.printf("[IMUDIAG] err=%s\n", errName(diag_err));
    }

    // Pipeline observability (F01→F02→F03 task-table integration,
    // 2026-10-08): one bench line proves the whole chain. motion_seq = F02's
    // mailbox sequence (windows being published), drops = the D10 queues
    // keeping up, activity/conf = F03 having consumed a window and stepped
    // the classifier. Const accessors only — LatestValue/SpscQueue are
    // tear-free, so ProcessingTask and F03Pump are undisturbed. Replaces the
    // D16 [RAW] three-path matrix: its benchRawRead probes use raw Wire with
    // no mutex ("bench callers run on the loop task alone",
    // esp32_i2c_backend.h), which is unsafe once SensorTask reads the bus
    // concurrently — and the anomaly it was built for was closed by commit
    // edec981 (kept in wire_hal for single-task bench reuse).
    bw::ActivityEstimate est{};
    const unsigned long motion_seq = static_cast<unsigned long>(ctx.motion_features.sequence());
    const unsigned long imu_drops = static_cast<unsigned long>(ctx.imu_samples.drops());
    const unsigned long ppg_drops = static_cast<unsigned long>(ctx.ppg_samples.drops());
    if (ctx.activity_estimate.get(est)) {
        Serial.printf(
            "[PIPE] motion_seq=%lu activity=%s conf=%.2f t=%llu ms drops imu=%lu ppg=%lu\n",
            motion_seq, activityContextName(est.context), static_cast<double>(est.confidence),
            static_cast<unsigned long long>(est.t_ms), imu_drops, ppg_drops);
    } else {
        Serial.printf("[PIPE] motion_seq=%lu no estimate yet drops imu=%lu ppg=%lu\n", motion_seq,
                      imu_drops, ppg_drops);
    }

    // W2 P4 (2026-10-12): one line proves the offline M2 decision loop with
    // real neighbours — F04/F06 axes composed by F07 into the snapshot,
    // F23's per-source quality view, F10's alert level and F08's episode
    // status, all through ctx mailboxes. Const accessors only — tear-free,
    // the producing tasks are undisturbed. "none" = nothing published yet
    // (an ordinary boot state, never fabricated).
    bw::StateSnapshot snap{};
    if (ctx.state_snapshot.get(snap)) {
        bw::AlertStatus alert{};
        const bool has_alert = ctx.alert_status.get(alert);
        bw::EpisodeStatus episode{};
        const bool has_episode = ctx.episode_status.get(episode);
        bw::SourceQuality quality{};
        const bool has_quality = f23::latestSourceQuality(quality);
        Serial.printf("[LOOP] t=%llu ms state=%s wear=%s conf=%.2f in_state=%lu ms "
                      "q imu=%s ppg=%s alert=%s alert_drops=%lu\n",
                      static_cast<unsigned long long>(snap.t_ms),
                      alertnessStateName(snap.alertness), wearStateName(snap.wear),
                      static_cast<double>(snap.confidence),
                      static_cast<unsigned long>(snap.in_state_ms),
                      has_quality ? dataQualityName(quality.imu) : "none",
                      has_quality ? dataQualityName(quality.ppg) : "none",
                      has_alert ? alertLevelName(alert.level) : "none",
                      static_cast<unsigned long>(f10::droppedEvents()));
        if (has_episode) {
            Serial.printf("[EPIS] t=%llu ms phase=%s conf=%.2f\n",
                          static_cast<unsigned long long>(episode.t_ms),
                          episodePhaseName(episode.phase), static_cast<double>(episode.confidence));
        }
    } else {
        Serial.printf("[LOOP] no snapshot yet (F07 gated: boot / no inputs)\n");
    }
}

// --- W3 P4 (2026-10-13): the events dataflow's composition glue --------------
// Both types below are pure forwarders: the composition root is the only place
// that names concrete classes, and wiring carries no logic (01 §1 rule 3) —
// construction and injection only, same class of glue as wire_input's
// command routing (backlog 15).

// F17's fan-out is a list of bw::IEventSink, while F14's event entry point is
// the plain function onEvent (D2: bw_core value types only, no subscriber
// type exists in the contract) — this adapts one to the other so a published
// event that may need confirmation reaches the manager (F14 §15).
struct ConfirmTap final : bw::IEventSink {
    bw::Result<void> publish(const bw::Event& e) override {
        f14::onEvent(e);
        return {};
    }
};

// W3 H8 / M3 bench visibility: one [EVT] row per appended event (seq, boot,
// type, detector, confidence + F22's reason text). Printed from the event
// pump (core 1, priority 2): the line is short and events are rate-limited
// by design (F17 §13), so the 115200 TX ring absorbs it without ever
// blocking a producer — producers only touch the bounded queue.
struct EvtPrinter final : bw::IEventSink {
    bw::Result<void> publish(const bw::Event& e) override {
        if (rawCaptureActive()) {
            return {};  // A5: [EVT] muted during raw capture; the event is unaffected
        }
        char why[bw::explain::kExplainShortBufSize] = {};
        // WireExplain is app::WireExplain (the seam is composition-root
        // scoped, unlike the f01..f28 wiring namespaces) — we are inside
        // namespace app here.
        const size_t why_len = WireExplain::explainShort(e.reasons, why, sizeof(why));
        Serial.printf("[EVT] t=%lu seq=%lu boot=%lu type=%u det=%u conf=%.2f %s\n",
                      static_cast<unsigned long>(e.t_start_ms), static_cast<unsigned long>(e.seq),
                      static_cast<unsigned long>(e.boot_id), static_cast<unsigned>(e.type),
                      static_cast<unsigned>(e.detector_id), static_cast<double>(e.confidence),
                      (why_len > 0) ? why : "");
        return {};
    }
};

// Static instances: no heap, built before app_main like the feature singletons
// (app.cpp static-storage pattern); attached in run()'s DI block below.
ConfirmTap s_confirm_tap{};
EvtPrinter s_evt_printer{};

// --- W4 P4 (2026-10-16): integrator service pump (F20 sync + F16 history) --
// Neither feature creates a task by design: F20 D6 exposes pumpOnce() for the
// composition root's cadence, F16 D10 exposes historyTick() for an existing
// low-priority task. One task drives both because they share the same
// non-real-time, flash-adjacent profile (DEC-008: keep them off core 1).
// [EXPERIMENTAL] Stack: pumpOnce holds <= kMaxEventsPerRead (16) events
// transiently (768 B) plus codec scratch (<= 512 B); historyTick drains
// <= kAggMaxEventsPerTick (8) events (384 B) + finalize frames. 4096 B leaves
// ~2x headroom over the ~1.5 KB worst measured frame chain; raise if the H7
// bench high-water mark says otherwise.
constexpr uint32_t kServicePumpStackBytes = 4096;
// [EXPERIMENTAL] Same rung as StorageTask/CommsTask (runtime.md 3.2, low on
// core 0): sync and history are best-effort telemetry — a late tick is a
// diagnostic (F23), never an alert-path risk.
constexpr UBaseType_t kServicePumpPriority = 2;
constexpr BaseType_t kServicePumpCore = 0;
// [EXPERIMENTAL] 2 s period bounds worst-case sync latency (phone CURSOR
// write -> next engine step) while costing ~0 CPU with the link down (the
// engine is a pull-style no-op then). F20's contract asks for "every few
// seconds when connected" (wire_sync.h) — 2 s satisfies it; the 60 s history
// cadence is the caller-paced bw::history::kAggTickPeriodMs contract below.
constexpr uint32_t kServicePumpPeriodMs = 2000;

// The pump body. Every call is bounded and non-blocking (00 7.2): pumpOnce is
// one engine step; F16 restore advances at this 2 s cadence until ready, then
// regular aggregation returns to its 60 s cadence.
void servicePumpTask(void* arg) {
    tasks::waitForStartup();
    (void)arg;
    uint32_t last_history_ms = millis();
    bool history_ready_reported = false;
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const uint32_t late_ms = tasks::wakeLatenessMs(last_wake);
        const bw::TimeMs now = millis();
        f20::pumpOnce(now);
        (void)f24::modePumpOnce();  // W6 P4: mode/power profile (2 s cadence)
        const bool history_was_ready = f16::historyReady();
        const bool history_due =
            static_cast<uint32_t>(now - last_history_ms) >= bw::history::kAggTickPeriodMs;
        if (!history_was_ready || history_due) {
            (void)f16::historyTick();
            if (!history_ready_reported && f16::historyReady()) {
                history_ready_reported = true;
                last_history_ms = millis();
                if (!rawCaptureActive()) {
                    Serial.printf("[STARTUP] history-ready t=%lu ms\n",
                                  static_cast<unsigned long>(last_history_ms));
                }
            } else if (history_was_ready) {
                last_history_ms = now;
            }
        }
        tasks::completeCycle(bw::diag::TaskSlot::ServicePump, late_ms);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kServicePumpPeriodMs));
    }
}

}  // namespace

void run() {
    // Serial is touched ONLY here, in the composition root: feature modules
    // conventions — hardware access stays in F01's HAL). The bench
    // diagnostics below are therefore legitimate here, and disappear with the
    // scaffolding they belong to.
    Serial.begin(kSerialBaud);
    if (!rawCaptureActive()) {  // A5: banner is a boot one-shot print
        Serial.print("anarolepsy bracelet firmware ");
        Serial.println(kFirmwareVersion);
    }

    // Static storage: AppContext is 5312 B (sizeof probe 2026-10-08) and
    // loopTask gets only 8192 B total — as a local it left 688 B free after
    // f01::wire alone, too thin for the remaining wire ladder and its printf
    // frames (a local underflow would smash heap below the stack; the ISR
    // assert of 2026-10-08 showed how that ends). Static = no heap, built
    // before app_main, same pattern as the feature singletons.
    static AppContext ctx{};
    ctx.clock = nullptr;  // assigned by f01::wire() — D9/DEC-018
    ctx.firmware_version = kFirmwareVersion;
    ctx.faults = &s_faults;

    // Hardware first in 01 §2's fixed order (hardware → processing →
    // intelligence → decision); call order per wiring/README.md. Bring-up
    // never blocks: an absent IMU/PPG/display is a normal boot (spec §10) and
    // each begin() result is already reported above.
    //
    // Task table (F01 D15) instantiated 2026-10-08: every wire() below now
    // creates its feature's task after construction — f01 SensorTask (20 ms,
    // IMU/PPG → D10 queues), f02 ProcessingTask (1 s windows →
    // motion_features/ppg_features mailboxes), f03 F03Pump (1 s →
    // activity_estimate mailbox). AppContext (queues + mailboxes) is fully
    // constructed before the first wire(), and every "consumer before
    // producer" edge is a tested state (empty pop returns false,
    // hasValue()==false, sequence()==0), so task start order cannot break.
    // The remaining pairs (f04–f10) are construction + accessor only — their
    // pumps arrive with their features.
    // W6 P4: F26 first, so ctx.faults becomes the fault registry and every
    // feature wired below caches the registry reporter (no bench reporter).
    f26::wire(ctx);
    f01::wire(ctx);
    f02::wire(ctx);  // signal processing (processing): samples → features
    f03::wire(ctx);  // activity classification (intelligence)
    f04::wire(ctx);  // wear detection (intelligence)
    f05::wire(ctx);  // ML model slot — no-op until the first production model (M5)
    // F06 wired in the services slot (after F13) so its baseline provider can
    // be injected at construction (W6 P4).
    (void)ctx;
    // W1 (2026-10-11): f07 after its producers' wire slots; it reads the
    // mailboxes at tick time, so empty producers are the tested safe state.
    // F07 starts after ALL ports are bound below (no task/port-write race).
    // W2 P4 (2026-10-12): decision slot completed — f08 between f07 (its
    // state_snapshot producer) and f09/f10 (README order F07–F10).
    f08::wire(ctx);                           // sleep episode detector (decision): 1 Hz state poll
    f09::wire(ctx);                           // cataplexy candidate detector (decision)
    f10::wire(ctx, &f24::modeCommandSink());  // sole mode-queue producer: AlertTask
    // W3 P4 (2026-10-13): F14 per its own wiring contract ("decision band,
    // F07–F10 row", wire_confirm.h) so the manager exists before f11's
    // InputTask can emit a ConfirmEvent/RejectEvent. Its ack seam is the
    // ctx.alert_status mailbox step(ctx) already reads (D3) — no extra
    // binding. No task of its own: stepped from the bench loop below.
    f14::wire(ctx);  // user event confirmation (decision): alerts → prompts → labels
    f11::wire(ctx);  // user interaction (input): buttons → gestures → commands
    // W2 P4: services slot (F11–F18, F22–F26 per README) — f12 after f10
    // (binds alert_status/state_snapshot) and f01 (display accessors);
    // f23 after f01/f02 (sensor statuses + feature mailboxes). f28 stays
    // last (comms/tooling slot).
    // Row ③ display probe (W2 P4): one-shot BEFORE f12::wire — the panel is
    // valid (f01 ran) and no other writer exists yet, so this clear+flush
    // hands the panel over cleanly. Ok = panel answered, NotFound = absent
    // (boot continues, spec §10), anything else = bus trouble. After this,
    // F12's UiTask is the panel's only writer (the bench never redraws).
    {
        bw::IDisplayCanvas& probe_canvas = f01::display();
        probe_canvas.clear();
        const bw::Result<void> probe_flush = probe_canvas.flush();
        if (!rawCaptureActive()) {  // A5: boot one-shot print
            Serial.printf("[BENCH] display probe disp=%s\n", errName(probe_flush.err));
        }
    }
    f12::wire(ctx);  // OLED user interface: frames from the ctx mailboxes
    // W3 P4 (2026-10-13): services slot continued — storage mounts before the
    // logger wires, so the store is ready the moment f17::attachStore runs in
    // the DI block below (events queued meanwhile are absorbed by F17's
    // bounded queue; attach order is contractually free, F17 §15.5).
    f18::wire(ctx);  // local storage (services): LittleFS mount + StorageTask (core 0)
    // f17::wire must precede EVERY f17::sink()/attach* call below: the logger
    // binds ctx.clock/ctx.faults on first construction and later calls ignore
    // the arguments (wire_eventlog.cpp loggerImpl), so a sink() touch before
    // this line would leave the logger permanently clockless.
    f17::wire(ctx);  // event logging (services): Event{Boot} + pump (core 1)
    // W4 P4 (2026-10-16): F16 per its wiring contract — after f18 (stores
    // mounted) and f17 (Boot event exists). wireHistoryInit() only schedules
    // restore; bounded probe/restore/rebuild work runs after task release from
    // ServicePump, so history cannot hold the startup barrier.
    f16::wireHistoryInit(ctx, &f18::eventStore(), &f18::kvStore());
    // W6 P4: personalized baseline (loads from KV, publishes restored profile).
    f13::wireWithStore(ctx, &f18::kvStore());
    if (!rawCaptureActive()) {
        Serial.printf("[STARTUP] baseline-ready t=%lu ms\n", static_cast<unsigned long>(millis()));
    }
    // W6 P4: inject F13's provider into F06 at construction time.
    f06::wire(ctx, &f13::provider());
    // W6 P4: operating modes (command relay on its own SPSC seam).
    f24::wireModesInit(ctx, &f01::battery());
    f23::wire(ctx);  // sensor-quality diagnostics: 1 Hz composer + health
    // W6 P4: bind F26 (CR-030) into F23's HealthSnapshot read path.
    f23::attachReliabilityProvider(&f26::stateProvider());
    // W4 P4: comms slot (README order F19/F20, F28) — f19 first: f20 binds
    // the ISyncTransport seam f19::wire() just constructed (CR-019). On the
    // host both are honest no-ops (no radio there, wire_ble.h).
    f19::wire(ctx);  // BLE GATT server + CommsTask (core 0, runtime.md 3.2)
    f20::wire(f19::syncTransport(), &f18::eventStore());  // sync engine over F19 + F18
    f28::wire(ctx);  // research capture (tooling): flag-gated, default OFF
    // W6 P4: F26 attachments before startSupervisor (KV/storage/BLE/event).
    f26::attachKeyValueStore(&f18::kvStore());
    f26::attachStorageHealth(&f18::healthProvider());
    f26::attachBleLink(f19::linkProvider());
    f26::attachEventSink(f17::sink());
    // Dependency injection that needs ctx fields to exist (F07 §15 / f10::
    // attachOutputs precedent): f07's pump consumes wear_estimate +
    // alertness_probabilities and publishes state_snapshot. Pre-call state is
    // honest: the pump stays Unknown + SensorUnavailable and publishes nothing.
    f07::attachMailboxes(&ctx.wear_estimate, &ctx.alertness_probabilities, &ctx.state_snapshot);
    // W4 P4 (F07 P4b rider, D16): bind F23's latched quality accessor — one
    // function pointer, no new ctx field. Before F23's first publish the
    // provider answers false, so F07 keeps the honest Unknown/derivation
    // path (never fabricates sensor health).
    f07::attachQualityProvider(f23::latestSourceQuality);
    f07::attachModeProvider(f24::modeProvider());
    // W2 P4: f08's episode mailbox (CR-007) and the boot-time event-sink
    // site — the NULL placeholder this line pinned until F17 landed (W3) is
    // now replaced by the real logger below (audit M-1 made F08's pending
    // FIFO flush attach-order independent, so nothing is lost in between).
    f08::attachEpisodeStatus(&ctx.episode_status);

    // --- W3 P4 (2026-10-13): events & storage dataflow (F17 spec §15.5 order)
    // Store probe first: attachStore runs the boot_id/seq recovery before any
    // drain; producers may attach in any order (bounded queues absorb gaps).
    // F18 has already mounted/recovered its segment index. Pass the persisted
    // tail sequence as an app-local hint so F17 can recover boot_id with one
    // tail read instead of probing the full uint32 seq range at startup.
    const uint32_t last_event_seq_hint = f18::eventStore().segments().lastEventSeq();
    if (!rawCaptureActive()) {
        Serial.printf("[STARTUP] event-tail-hint=%lu t=%lu ms\n",
                      static_cast<unsigned long>(last_event_seq_hint),
                      static_cast<unsigned long>(millis()));
    }
    f17::attachStore(&f18::eventStore(), last_event_seq_hint);
    if (!rawCaptureActive()) {
        Serial.printf("[STARTUP] event-recovery-ready t=%lu ms\n",
                      static_cast<unsigned long>(millis()));
    }
    f07::attachEventSink(f17::sink());  // state transitions → EventLog
    f08::attachEventSink(f17::sink());  // sleep episodes → EventLog (replaces the W2 NULL)
    f09::attachEventSink(f17::sink());  // cataplexy candidates → EventLog
    // F17 §15.5 prescribes this re-call; wire_alert.cpp attached
    // (led, buzzer, nullptr) at f10::wire per F10's documented deferred-DI
    // design ("null until F17", NotReady + counted pending, never a silent
    // loss). Safe at this point: no level can be raised before f07 publishes
    // a snapshot (policy stays Normal — tested F10 §9), and loopTask shares
    // core 1 with AlertTask, so attachOutputs' three plain member stores
    // (alert_engine.h:52) cannot run concurrently with a tick.
    f10::attachOutputs(&f01::led(), &f01::buzzer(), f17::sink());
    f07::wire(ctx);  // all F07 ports now immutable for StateTask's lifetime
    f14::attachEventStore(&f18::eventStore());  // confirmed/rejected labels → F18
    f14::attachEventSink(f17::sink());          // F14's UserMarked notifications → F17
    // F14's event input: the ConfirmTap adapter above bridges F17's IEventSink
    // fan-out to the plain onEvent entry point; EvtPrinter is the W3 H8 bench
    // line. 2 of 8 subscriber slots (kMaxEventSubscribers).
    (void)f17::attachSubscriber(&s_confirm_tap);
    (void)f17::attachSubscriber(&s_evt_printer);
    // CR-016 completed: F12's UiTask polls this once per frame and flips the
    // D6 gate setPromptAvailable(phase != Idle); nullptr until this line was
    // the W2 behavior (Prompt screen unreachable).
    f12::attachConfirmPromptProvider(&f14::manager());

    // --- W4 P4 (2026-10-16): F19 P4 seams + the integrator service pump ----
    // Provider bindings (wire_ble.h: nullptr providers are honest — F19 counts
    // refusals and fabricates nothing):
    //  - state   : F07's StateProvider reads the state_snapshot mailbox
    //              (LatestValue::get is tear-free cross-task, so CommsTask on
    //              core 0 may read what StateTask on core 1 publishes);
    //  - battery : F01's IBatteryMonitor;
    //  - events  : F18's LittleFsEventStore — the Event characteristic drains
    //              it through IEventStore::readFrom (contract-level reads only,
    //              F19 never touches F18 internals);
    //  - features: nullptr — no production IFeatureProvider exists (F02's
    //              mailboxes are not an IFeatureProvider); the characteristic
    //              answers "no data", the honest empty state;
    //  - settings: nullptr — no production ISettings (CR-003's table is empty
    //              until F15 lands the settings feature);
    //  - commands: nullptr — ctx.commands is an SPSC queue whose sole producer
    //              is F11's InputTask; a second producer (BLE) would be a data
    //              race. BLE-originated commands need an MPMC queue or an F11
    //              relay — backlog item, not a P4 shortcut.
    f19::attachProviders(&f07::provider(), /*features=*/nullptr, &f01::battery(),
                         /*settings=*/nullptr, /*commands=*/nullptr, &f18::eventStore());
    // Device Information strings (F19 D4, spec 18): hw_rev is a build fact
    // (kHwRevision above); the schema string is COMPOSED from the contracts'
    // own kSchemaVersion constants so it cannot silently drift from the wire
    // formats the firmware actually speaks (CR-002 events, F16 summaries,
    // CR-019 sync status).
    static char s_schema_versions[64];
    std::snprintf(s_schema_versions, sizeof(s_schema_versions), "evt=%u,ds=%u,ss=%u",
                  static_cast<unsigned>(bw::Event::kSchemaVersion),
                  static_cast<unsigned>(bw::history::DailySummary::kSchemaVersion),
                  static_cast<unsigned>(bw::SyncStatus::kSchemaVersion));
    f19::attachIdentity(kHwRevision, s_schema_versions);
    // The service pump (F20 D6 + F16 D10): one core-0 task pacing pumpOnce
    // every kServicePumpPeriodMs; F16 restore uses that cadence until ready,
    // then historyTick returns to kAggTickPeriodMs. Created after wiring so
    // its first tick sees live seams.
    // W6 P4: start F26 supervisor only after all attachments above.
    f26::startSupervisor(ctx);
    tasks::checkCreated(xTaskCreatePinnedToCore(servicePumpTask, "ServicePump",
                                                kServicePumpStackBytes, nullptr,
                                                kServicePumpPriority, nullptr, kServicePumpCore),
                        bw::diag::TaskSlot::ServicePump, ctx.faults);
    // W6 P4: core-0 baseline learner pump (F13).
#if defined(ARDUINO)
    tasks::checkCreated(
        xTaskCreatePinnedToCore(f13::pumpTask, "F13Pump", f13::kBaselinePumpStackBytes, &ctx,
                                f13::kBaselinePumpPriority, nullptr, f13::kBaselinePumpCore),
        bw::diag::TaskSlot::Baseline, ctx.faults);
#endif

    if (!rawCaptureActive()) {  // A5: boot one-shot print
        Serial.printf("[BENCH] F01 Bench A — diag every %u ms, "
                      "led: idle=off big=red small=green both=blue, "
                      "beep every %u ms%s\n",
                      static_cast<unsigned>(kDiagPeriodMs),
                      static_cast<unsigned>(kBuzzerBeepPeriodMs),
                      kBuzzerHeartbeatEnabled ? "" : " (MUTED - presentation build)");
    }

    uint32_t last_diag_ms = millis() - kDiagPeriodMs;        // first line immediately
    uint32_t last_beep_ms = millis() - kBuzzerBeepPeriodMs;  // first beep now
    uint32_t beep_started_ms = 0;
    bool beep_on = false;

    // Button → LED test state: -1 = off, otherwise index into kBenchColours.
    f01::led().set(bw::Rgb{0, 0, 0});
    int led_state_idx = -1;
    const char* led_state_name = "OFF";

    bool prev_big = f01::buttons().isPressedRaw(bw::ButtonId::Big);
    bool prev_small = f01::buttons().isPressedRaw(bw::ButtonId::Small);

    // W2 P4 (2026-10-12): f10 now owns the LED/buzzer while an alert level
    // is raised (attachOutputs in wire_alert.cpp) — the bench must never
    // fight the pattern player. `bench_released` hands the actuators over
    // exactly once; the mirror/beep resume at Normal.
    bool bench_released = false;

    // Final startup publication: no task may observe partially attached ports.
    // Safe mode is report-only in W6 (G1/P4); no live task unwiring or alert gate.
    if (!rawCaptureActive()) {
        Serial.printf("[STARTUP] tasks-released t=%lu ms\n", static_cast<unsigned long>(millis()));
    }
    tasks::startup.release();

    // Bench loop after the task table (2026-10-08): SensorTask,
    // ProcessingTask and F03Pump run beside this loop on core 1, so nothing
    // here may read sensors — buttons/LED/beeper are GPIO, the diagnostics go
    // through the mutex'd I2cBus. Wrap-safe unsigned arithmetic, no blocking
    // delay() — every wait is a vTaskDelay tick (00 §7.2).
    for (;;) {
        const uint32_t now = millis();

        // Buttons: print only on an edge. Row ④ passes when NOTHING appears
        // while the buttons are untouched, and one line per press otherwise.
        const bool big = f01::buttons().isPressedRaw(bw::ButtonId::Big);
        const bool small = f01::buttons().isPressedRaw(bw::ButtonId::Small);
        if (big != prev_big || small != prev_small) {
            if (!rawCaptureActive()) {  // A5
                Serial.printf("[BTN] t=%lu ms big=%d small=%d\n", static_cast<unsigned long>(now),
                              big ? 1 : 0, small ? 1 : 0);
            }
            prev_big = big;
            prev_small = small;
        }

        // W2 P4: actuator hand-over while an alert level is raised (see
        // bench_released above). Const get(), tear-free.
        bw::AlertStatus bench_alert{};
        const bool alert_active =
            ctx.alert_status.get(bench_alert) && bench_alert.level != bw::AlertLevel::Normal;
        if (alert_active) {
            if (!bench_released) {
                bench_released = true;
                f01::led().set(bw::Rgb{0, 0, 0});
                led_state_idx = -1;
                led_state_name = "OFF";
                if (beep_on) {
                    f01::buzzer().set(false, 0);
                    beep_on = false;
                }
            }
        } else {
            bench_released = false;
        }

        if (alert_active) {
            // f10's pattern player drives both actuators; the bench keeps
            // its hands off until the level returns to Normal.
        } else if (kPinFinderEnabled) {
            // Rows ④/⑤ investigation: one event per call keeps the log
            // readable (pin + level, or the input edge that a press produced).
            char finder_msg[96] = {};
            // A5: pinFinderStep keeps running while muted — only its print is gated.
            if (f01::pinFinderStep(now, finder_msg, sizeof(finder_msg)) && !rawCaptureActive()) {
                Serial.print("[PINFIND] ");
                Serial.println(finder_msg);
            }
        } else {
            // Developer's test (2026-10-02): LED mirrors the buttons so the
            // result is visible without a serial terminal — idle = off,
            // BIG = red, SMALL = green, both = blue. Written only on a state
            // change (no 100 Hz PWM churn), and a stuck input lights the LED
            // by itself: green with nobody touching the board = GPIO27 still
            // shorted, red = GPIO26 stuck pressed.
            int want_idx = -1;
            if (big && small) {
                want_idx = 2;  // blue
            } else if (big) {
                want_idx = 0;  // red
            } else if (small) {
                want_idx = 1;  // green
            }
            if (want_idx != led_state_idx) {
                led_state_idx = want_idx;
                led_state_name = (want_idx < 0) ? "OFF" : colourName(static_cast<size_t>(want_idx));
                f01::led().set(want_idx < 0 ? bw::Rgb{0, 0, 0} : kBenchColours[want_idx]);
            }
        }

        if (kBuzzerHeartbeatEnabled && !alert_active && !beep_on &&
            static_cast<uint32_t>(now - last_beep_ms) >= kBuzzerBeepPeriodMs) {
            last_beep_ms = now;
            beep_on = true;
            beep_started_ms = now;
            f01::buzzer().set(true, kBuzzerBeepFreqHz);
            if (!rawCaptureActive()) {  // A5: beep still sounds, only its print is muted
                Serial.printf("[BUZZ] t=%lu ms beep %u Hz\n", static_cast<unsigned long>(now),
                              static_cast<unsigned>(kBuzzerBeepFreqHz));
            }
        }
        if (beep_on && static_cast<uint32_t>(now - beep_started_ms) >= kBuzzerBeepMs) {
            f01::buzzer().set(false, 0);
            beep_on = false;
        }

        if (static_cast<uint32_t>(now - last_diag_ms) >= kDiagPeriodMs) {
            last_diag_ms = now;
            printDiagnostics(ctx, kPinFinderEnabled ? "FINDER" : led_state_name);
        }

        // W3 P4 (2026-10-13): F14 owns no task (plan §15) — one pump per loop
        // tick. Its timing constants are seconds-scale (defer/offer/timeout),
        // so 10 ms tick jitter is irrelevant; step() reads only the
        // alert_status mailbox + clock and is O(kMaxPending ≤ 4).
        f14::step(ctx);

        vTaskDelay(pdMS_TO_TICKS(kLoopTickMs));
    }
}

}  // namespace app
