#include "phone_harness.h"

#include <cstdio>
#include <cstring>

namespace f20test {
namespace {

using bw::sync::CursorRequest;
using bw::sync::decodeChunkFrame;
using bw::sync::encodeCursorFrame;
using bw::sync::kCursorFrameBytes;

std::string line(const char* prefix, std::uint32_t from_seq, std::uint16_t max,
                 std::uint32_t ack_seq) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s from=%u max=%u ack=%u", prefix,
                  static_cast<unsigned>(from_seq), static_cast<unsigned>(max),
                  static_cast<unsigned>(ack_seq));
    return std::string(buf);
}

}  // namespace

bool PhoneHarness::requestFrom(std::uint32_t from_seq, std::uint16_t max) {
    CursorRequest request;
    request.from_seq = from_seq;
    request.max = max;
    // A real companion piggybacks its cumulative ack on the next CURSOR (§3.1:
    // fields are independent) — that is how an ack lost to a disconnect is
    // recovered on reconnect.
    request.ack_seq = hw_;
    bw::Result<std::size_t> r = writeCursor(bw::SyncChannel::Control, request);
    if (r.ok()) {
        if (from_seq > 0 && hw_ + 1 < from_seq) {
            hw_ = from_seq - 1;  // the request asserts everything below is stored (§4.1)
        }
        if (request.ack_seq != 0) {
            last_acked_ = request.ack_seq;
        }
        transcript_.push_back(line("P> CURSOR", from_seq, max, request.ack_seq));
    }
    return r.ok();
}

bool PhoneHarness::statusQuery() {
    CursorRequest request;  // all-zero = status query (§3.1)
    bw::Result<std::size_t> r = writeCursor(bw::SyncChannel::Control, request);
    if (r.ok()) {
        transcript_.push_back(line("P> CURSOR(status-query)", 0, 0, 0));
    }
    return r.ok();
}

bw::Result<std::size_t> PhoneHarness::writeCursor(bw::SyncChannel ch,
                                                  const CursorRequest& request) {
    std::uint8_t frame[kCursorFrameBytes];
    encodeCursorFrame(request, frame);
    if (!transport_.pushToDevice(ch, frame, sizeof frame)) {
        return {bw::Err::Full, 0};
    }
    return {bw::Err::Ok, sizeof frame};
}
std::size_t PhoneHarness::drain(std::size_t max_frames) {
    std::uint8_t frame[bw::kMaxSyncFrameBytes];
    bw::SyncChannel ch = bw::SyncChannel::Control;
    std::size_t processed = 0;
    while (processed < max_frames) {
        const std::size_t len = transport_.takePhoneFrame(&ch, frame, sizeof frame);
        if (len == 0) {
            break;
        }
        ++processed;
        if (ch == bw::SyncChannel::Data) {
            bw::Event decoded[bw::sync::kMaxEventsPerChunk];
            std::uint16_t chunk_no = 0;
            bw::Result<std::size_t> rec =
                decodeChunkFrame(frame, len, &chunk_no, decoded, std::size(decoded));
            char buf[96];
            if (!rec.ok()) {
                ++corrupt_chunks_;  // §4.7: drop, no ack, device resends on timeout
                std::snprintf(buf, sizeof buf, "D< CHUNK#%u corrupt(dropped)",
                              static_cast<unsigned>(chunk_no));
                transcript_.push_back(std::string(buf));
                continue;
            }
            std::uint32_t first = 0;
            std::uint32_t last = 0;
            for (std::size_t i = 0; i < rec.value; ++i) {
                const bw::Event& e = decoded[i];
                if (i == 0) {
                    first = e.seq;
                }
                last = e.seq;
                if (stored_.insert(e.seq).second) {
                    ++store_writes_;  // durable upsert commit (§4.4 idempotency)
                    // The ack is the CONTIGUOUS committed prefix: a hole (lost or
                    // reordered chunk) holds the ack behind, so the device's resend
                    // fills it before the marker may move (D4, F01/F03).
                    while (stored_.count(hw_ + 1) != 0) {
                        ++hw_;
                    }
                } else {
                    ++dup_deliveries_;  // duplicate delivered, store unchanged
                }
            }
            std::snprintf(buf, sizeof buf, "D< CHUNK#%u seq=%u..%u (%u ev)",
                          static_cast<unsigned>(chunk_no), static_cast<unsigned>(first),
                          static_cast<unsigned>(last), static_cast<unsigned>(rec.value));
            transcript_.push_back(std::string(buf));
        } else {
            if (len >= 11 && frame[0] == 1 && frame[1] == 1) {
                ++status_frames_;
                std::memcpy(&last_status_last_seq_, frame + 2, 4);
                std::memcpy(&last_status_last_synced_, frame + 6, 4);
                last_status_flags_ = frame[10];
                char buf[96];
                std::snprintf(buf, sizeof buf, "D< STATUS last=%u synced=%u flags=0x%02X",
                              static_cast<unsigned>(last_status_last_seq_),
                              static_cast<unsigned>(last_status_last_synced_),
                              static_cast<unsigned>(last_status_flags_));
                transcript_.push_back(std::string(buf));
            }
        }
    }
    return processed;
}

std::size_t PhoneHarness::drainAndAck() {
    const std::size_t processed = drain();
    if (suppress_acks_ || hw_ == 0 || hw_ == last_acked_) {
        return processed;  // nothing new: the companion only re-acks on progress
    }
    // Cumulative ack AFTER the (already committed) upsert, exactly per D4.
    CursorRequest ack;
    ack.from_seq = 0;
    ack.max = 0;
    ack.ack_seq = hw_;
    bw::Result<std::size_t> r = writeCursor(bw::SyncChannel::Control, ack);
    if (r.ok()) {
        last_acked_ = hw_;
        ++acks_written_;
        transcript_.push_back(line("P> CURSOR(ack)", 0, 0, hw_));
    }
    return processed;
}

void PhoneHarness::simulateCompanionReboot() {
    stored_.clear();  // in-RAM view lost; hw_ (persisted) is the resume point (§4.6)
    suppress_acks_ = false;
}

void PhoneHarness::setSuppressAcks(bool suppress) {
    suppress_acks_ = suppress;
}

}  // namespace f20test
