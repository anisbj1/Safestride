#pragma once

// bw/storage/constants.h — F18's named constants (spec F18 §8 and §15.4).
// Every value carries one tag ([HW]/[DERIVED]/[EXPERIMENTAL]/[USER]) and a why
// (00 §7.5). Values tagged [EXPERIMENTAL] are starting points to tune with data
// (F28/F29); changing them changes the on-disk format only where noted, so the
// on-format-sensitive ones live next to the code that serializes them.

#include <cstddef>
#include <cstdint>

namespace bw::storage {

// --- segment log geometry (spec §8, D1/D4) -----------------------------------

// [EXPERIMENTAL] 16 KB segment files. Why: a multiple of the LittleFS erase
// block (4 KB), so a segment maps to whole blocks; bounds the boot recovery
// scan to <= kSegmentSizeBytes per segment (spec §8). 16384/60 = 272 event
// records per segment.
constexpr uint32_t kSegmentSizeBytes = 16384;

// [EXPERIMENTAL] Event-log capacity 256 KB (spec §8) => 256 KB / 16 KB = 16
// fixed segment slots (evt_000.bin .. evt_015.bin). Fixed slots make rotation
// a file reuse, never an unbounded create, so the log cannot outgrow its
// partition share and ENOSPC on segments is decidable from the RAM index.
constexpr uint32_t kMaxSegments = 16;

// [DERIVED] Segment file header: magic + format version + serial (creation
// order, the scan's ordering key — first_seq is NOT persisted because it is
// only known after the first event lands and updating a written header would
// break the append-only invariant of D4).
constexpr uint32_t kSegmentHeaderBytes = 16;

// 'F18S' little-endian. A segment whose header fails magic/version is skipped
// and removed at boot (D4) — never guessed at.
constexpr uint32_t kSegmentMagic = 0x53383146;
constexpr uint8_t kSegmentFormatVersion = 1;

// [EXPERIMENTAL] Resync candidate budget per mount (D4): bounds boot time
// when a segment is heavily corrupted; beyond it the rest of that segment is
// skipped (segments_skipped++) and boot continues — never a brick (§10).
constexpr uint32_t kMaxCorruptResyncsPerBoot = 32;

// [EXPERIMENTAL] Label-patch cache size (D4 consequence: labels are tail
// patch records, applied at read time). 16 covers the realistic confirm/
// reject backlog of a session; older patches beyond the cache are dropped
// from RAM but remain on disk (documented limitation, status log).
constexpr size_t kLabelPatchCacheEntries = 16;

// --- op queue (D3) ------------------------------------------------------------

// [EXPERIMENTAL] Depth of the append/label op queue between producers and the
// StorageTask pump. 16 absorbs a plausible F17 burst before back-pressure and
// costs 16 * sizeof(StorageOp) (~1 KB) of the 8 KB RAM budget (spec §11).
constexpr size_t kStorageQueueDepth = 16;

// [EXPERIMENTAL] Sync-cursor flush rate limit (D5): at most one SyncMarker
// record every 5 s while sync is advancing; a power cut loses at most this
// much cursor progress, which F20 re-syncs idempotently (DEC-011).
constexpr uint32_t kSyncCheckpointIntervalMs = 5000;

// --- KV store (D7) ------------------------------------------------------------

// [EXPERIMENTAL] Max key length in bytes (excluding NUL). Fits the stable
// SettingSpec `name` identifiers (02 §5) with headroom; keys are validated
// against a path-safe charset so a key can never escape the kv/ directory.
constexpr size_t kKvKeyMaxLen = 32;

// [DERIVED] KV blob envelope: magic + schema version + length + payload + CRC32.
// Max payload is bounded by the uint16 length field; the store refuses larger
// puts rather than silently truncating (00 §7.2).
constexpr uint16_t kKvMaxValueBytes = 512;

// --- window store (spec §8) ---------------------------------------------------

// [DERIVED] 60 s x 50 Hz x 6 channels x int16 = 36000 B (spec §8 kRawWindowBytes).
constexpr uint32_t kRawWindowBytes = 36000;

// [EXPERIMENTAL] Max stored windows, oldest-first eviction (spec §8). 8 x ~36 KB
// ~= 290 KB of the 768 KB partition share.
constexpr size_t kMaxStoredWindows = 8;

// --- IO -----------------------------------------------------------------------

// [DERIVED] Streaming chunk for CRC computation and window IO: bounds stack
// usage per call to this size regardless of payload length (no heap, 00 §7.2).
constexpr size_t kIoChunkBytes = 128;

// [DERIVED] Max path length our generated names need: "kv_" + a
// kKvKeyMaxLen key + ".tmp" + NUL = 40. Bounded stack buffers everywhere;
// keys never reach here unvalidated (kKvKeyMaxLen + charset check).
constexpr size_t kMaxPathBytes = 48;

// --- partitioning (§15.6, A-03) -----------------------------------------------

// [HW] LittleFS partition LABEL passed to LittleFS.begin(). MUST equal the
// Name column of the data row in firmware/partitions.csv (F18 owns its
// content): the Arduino default "spiffs" matches no row of our table, so on
// the bench every mount — and the F-03 format fallback, which can format
// nothing without a matching partition — failed with "partition spiffs could
// not be found" and events never survived a reboot (COM14, master @ 2a57192).
// The two must move together; pinned host-side by test_doc_claims' partition
// test.
constexpr const char* kLittleFsPartitionLabel = "storage";

// --- runtime (wire_storage.h owns the task constants) ------------------------

}  // namespace bw::storage
