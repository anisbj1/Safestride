#pragma once

// bw_sync test support — PhoneHarness: plays the companion (F20 spec §7).
// Receives device frames, CRC-checks chunks, upserts by seq into a set (the
// companion's durable high-water semantics, §4.4), validates marker semantics,
// and emits CURSOR requests/acks. Asserts zero loss / zero duplicates. Host
// test code: std containers are fine here (never shipped to firmware).

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <bw/events.h>
#include <bw/sync/chunk_packetizer.h>
#include <bw/testkit/host_only.h>

#include "lossy_transport_fake.h"

namespace f20test {

class PhoneHarness {
public:
    explicit PhoneHarness(LossyTransportFake& transport) : transport_(transport) {}

    // -- phone -> device (CURSOR writes, §3.1) --------------------------------
    // Request streaming from `from_seq` (max 0 = no request cap). Records a
    // transcript line; returns false if the fake's device inbox is full.
    bool requestFrom(std::uint32_t from_seq, std::uint16_t max = 0);
    bool statusQuery();  // all-zero CURSOR (§3.1)

    // -- device -> phone (drain the fake's phone-side queue) ------------------
    // Processes up to `max_frames` pending frames this call. Chunks: CRC-check
    // (bad CRC => dropped + counted, NO ack — §4.7), then idempotent upsert.
    // STATUS: flags/seqs recorded. Returns frames actually processed.
    std::size_t drain(std::size_t max_frames = 32);
    // Convenience: drain, then write the cumulative ack if new events landed
    // and acks are not suppressed (the companion acks AFTER its commit, D4).
    std::size_t drainAndAck();

    // Companion "reboot" (SYNC_PROTOCOL.md §4.6): clears the observed set
    // (the in-RAM view) and stops any scripted ack-loss (suppress_acks_ ->
    // false); the PERSISTED high-water hw_ plus the ack bookkeeping and
    // counters survive — hw_ is the resume point.
    void simulateCompanionReboot();
    void setSuppressAcks(bool suppress);  // F08: lose the ack on purpose

    // -- observations ----------------------------------------------------------
    std::uint32_t highWater() const { return hw_; }
    const std::set<std::uint32_t>& storedSeqs() const { return stored_; }
    std::size_t storeWrites() const { return store_writes_; }  // durable commits
    std::size_t acksWritten() const { return acks_written_; }  // cumulative acks sent
    std::size_t duplicateDeliveries() const { return dup_deliveries_; }
    std::size_t corruptChunks() const { return corrupt_chunks_; }
    std::size_t statusFrames() const { return status_frames_; }
    std::uint8_t lastStatusFlags() const { return last_status_flags_; }
    std::uint32_t lastStatusLastSeq() const { return last_status_last_seq_; }
    std::uint32_t lastStatusLastSynced() const { return last_status_last_synced_; }
    const std::vector<std::string>& transcript() const { return transcript_; }

private:
    bw::Result<std::size_t> writeCursor(bw::SyncChannel ch, const bw::sync::CursorRequest& request);

    LossyTransportFake& transport_;
    std::set<std::uint32_t> stored_;
    std::uint32_t hw_ = 0;          // committed high-water (persisted, §4.6)
    std::uint32_t last_acked_ = 0;  // last cumulative ack written (§4.3)
    std::size_t store_writes_ = 0;
    std::size_t acks_written_ = 0;
    std::size_t dup_deliveries_ = 0;
    std::size_t corrupt_chunks_ = 0;
    std::size_t status_frames_ = 0;
    std::uint8_t last_status_flags_ = 0;
    std::uint32_t last_status_last_seq_ = 0;
    std::uint32_t last_status_last_synced_ = 0;
    bool suppress_acks_ = false;
    std::vector<std::string> transcript_;
};

}  // namespace f20test
