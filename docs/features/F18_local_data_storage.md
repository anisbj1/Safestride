# F18 — Local data storage

> Track **D** · Milestone **M3** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Persist settings, the event log and event-associated sensor windows in flash without an internet or phone; keep high-volume raw data in RAM ring buffers only.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `Event` records (via F17)
- Settings/profile blobs via `IKeyValueStore`
- Raw window blobs (int16-packed) tied to `seq`

**Outputs (produced contracts)**
- `IEventStore` and `IKeyValueStore` implementations
- Storage health (free space, corruption counters) to F23

## 4. Dependencies
- `bw_core` only; flash access via a small `IFlashFs` abstraction internal to the lib.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Contain business rules.
- Write flash from real-time tasks.
- Delete unsynced events to make space without emitting a fault/event.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Event log format**
- Simplest: JSON lines file.
- Professional/scalable: Fixed-size CRC'd records in segment files, index by seq, ring overwrite of oldest synced segments.
- Trade-offs: Binary needs a decoder, but bounded and robust to power loss.
- **Choice:** Segmented binary log (DEC-007/012).

**D2. Power-loss safety**
- Simplest: Hope.
- Professional/scalable: Write record → CRC last; recover by scanning and truncating a torn tail.
- Trade-offs: A little recovery code.
- **Choice:** CRC-last + tail truncation on boot.

## 7. Module structure
- `LittleFsEventStore : IEventStore`
- `KvStore : IKeyValueStore`
- `WindowStore`
- `SegmentManager`
- `RecordCodec` (CRC16/32)
- `IFlashFs` + `LittleFsAdapter` + `RamFsFake`

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kSegmentSizeBytes` | 16384 | `EXPERIMENTAL` | A multiple of LittleFS block size; bounds recovery scan |
| `kMaxEventStorageBytes` | 256 KB | `EXPERIMENTAL` | Portion of assumed 4 MB flash; partition table decides |
| `kRawWindowBytes` | ≤ 36 KB each | `DERIVED` | 60 s × 50 Hz × 6 × int16 |
| `kMaxStoredWindows` | 8 | `EXPERIMENTAL` | ≈ 290 KB; oldest-first eviction |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Host with `RamFsFake`: append/read/seq continuity, CRC failure, torn-write recovery (inject failure at every byte), full-flash behavior, label updates.
- Torn-write recovery is proven by host fault injection; a device power-cut check is optional.

## 10. Failure behavior
- Corrupt segment ⇒ skip with counter; store never bricks boot; flash full ⇒ evict synced first, else drop oldest windows, else fault.

## 11. Budget
RAM < 8 KB; flash per Constants. The estimated static footprint is approximately 2.3 KB: queue 1024 B (`Op = 64 B` × 16), segment index 320 B (16 × 20 B), label-patch cache 128 B, KV scratch 256 B, window copy buffer 512 B, and health state 64 B. The StorageTask stack is 4096 B; LittleFS mount-time heap is measured separately.

## 12. Out of scope
- Encryption at rest (later CR).
- Cloud backup.

## 13. Open questions / assumptions
- A-03 flash size; partition table also needed by F30 (coordinate via CR).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Torn-write recovery tested at every byte offset on the host.

---

## 15. Implementation plan (Wave 3 · stage PLAN · 2026-10-12)

### 15.1 Contracts used — all pre-exist; this plan changes none of them
| Contract | Location (verified) | Use |
|---|---|---|
| `IEventStore` (`append`→seq, `readFrom`, `setLabel`, `markSyncedThrough`, `lastSyncedSeq`) | `bw/providers.h:86` | `LittleFsEventStore` implements |
| `IKeyValueStore` (`get`/`put`) | `bw/providers.h:95` | `KvStore` implements |
| `Err::Full` semantics (nothing written, input untouched) | `interfaces/core.md` §2 | `append` on full log/queue |
| `Event` = 48 B, layout pinned | CR-002 + `bw/contract_checks.h` | written verbatim, never changed |
| `IFaultReporter` | `bw/providers.h` / services.md §5 | storage faults (F26) |

- **SettingKey registry stays empty** — no `SettingKey` registered (CR-003 convention); `KvStore` is generic string-key storage only.
- **F17 is built in parallel**: zero F17 headers/dependencies. The producer-facing contract is the two interfaces above.
- **One genuine contract write**: CR-015 (storage health for F23) — §15.7. Nothing else needs a CR at this stage.

### 15.2 Module / file layout
```
firmware/lib/bw_storage/                     (Own)
  include/bw/storage/
    flash_fs.h        IFlashFs — internal abstraction (mount, size, read, write, append,
                      truncate, rename, remove). NOT a bw_core contract; F18-private.
    record_codec.h    RecordHeader {magic, type, len, reserved} + payload + CRC32-last;
                      encode/decode for Event(48 B), LabelPatch, SyncMarker.
                      CRC32 implemented here: grep of bw_core finds no CRC utility.
    segment_manager.h segment files, boot scan + torn-tail truncate + forward resync,
                      rotation, seq watermark, eviction, counters.
    event_store.h     LittleFsEventStore : bw::IEventStore (queue + pump, §15.3 D3)
    kv_store.h        KvStore : bw::IKeyValueStore (per-key files, §15.3 D7)
    window_store.h    WindowStore: write/evict of int16-packed windows tied to seq
                      (read access for F20 = future contract at F20's wave, NOT this plan)
    littlefs_adapter.h LittleFsAdapter : IFlashFs  (device build only)
  src/  *.cpp as above
firmware/test/bw_storage/                    (Own)
  test_bw_storage/
    ram_fs_fake.h        host IFlashFs fake with fault injection (op-fail, torn byte
                         offsets, forceUnformatted, failNextReadFor)
    test_record_codec.cpp · test_event_store.cpp · test_torn_write.cpp
    test_kv_store.cpp · test_window_store.cpp · test_doc_claims.cpp (audit F-05/F-09)
    test_main.cpp        Unity runner (registers every test)
firmware/src/app/wiring/wire_storage.{h,cpp} (Own) — StorageTask creation + constants only
firmware/partitions.csv                      (Own) — §15.6
```
Include style per F00 convention: `#include <bw/storage/event_store.h>`.

### 15.3 Design decisions (problem → alternatives → trade-offs → choice)

**D4. Record format / recovery (concretes D1/D2).** Segment files `evt_%03u.bin`, `kSegmentSizeBytes` 16 KB. Every write is an **append**: `Event` (48 B, verbatim), `LabelPatch {seq, label}` (labels are tail patches, no in-place rewrite), `SyncMarker {seq}` (D5). Header carries magic+type+len; **CRC32 written last** within each record. Boot scan: records validate while magic+len+CRC hold; first invalid **at the tail** → truncate to last good offset + `torn_tails_repaired++`; invalid record **with valid data after it** → byte-scan forward for the next magic whose record validates with seq continuity, `corrupt_records++` (cap `kMaxCorruptResyncsPerBoot`, then skip rest of segment + `segments_skipped++`). Because only appends happen, a torn write can only damage the tail; mid-segment damage = flash corruption, handled by resync + counter. A probe that FAILS to read (transient IO) is never treated as corruption: the scan skips that segment in place and keeps the file (F-04) — only a header successfully read and found invalid is removed. Store never bricks boot: worst case = skip that segment and continue at the next.

**D5. Sync cursor.** RAM is authoritative; flushed as `SyncMarker` tail record at most every `kSyncCheckpointIntervalMs`, plus on idle pump. Recovery takes the **last** marker ≤ tail seq ⇒ under-recovery after power cut, never over-recovery; F20 re-syncs idempotently (cursor+ack, DEC-011). `lastSyncedSeq()` = recovered marker value at boot.

**D8. `readFrom` semantics** (silent in the locked doc → defined here; F20 consumes as-is or raises a CR): inclusive `from_seq`, ascending, stops at `max` or tail; evicted/corrupt-skipped seqs are skipped (gap = normal); empty range → `Ok`, count 0; `Err::Corrupt` only if the store is unmountable.

**D9. Health exposure for F23** → CR-015, §15.7.

### 15.4 Named constants added by this plan (§8 values unchanged)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kRecordCrc` | CRC-32 IEEE, table-free | `[DERIVED]` | detects torn/corrupt records; bw_core has no CRC (grep verified) |
| `kSegmentHeaderBytes` | 16 | `[DERIVED]` | magic+version+serial (first_seq not persisted, D4); bounds the boot scan |
| `kStorageQueueDepth` | 16 | `[EXPERIMENTAL]` | ≥ one F17 burst before back-pressure; 16 × `Op = 64 B` ⇒ 1024 B RAM |
| `kSegmentManagerStateBudgetBytes` | 600 | `[DERIVED]` | runtime gate on `sizeof(SegmentManager)` (F-02): state ≈ 520 B measured ⇒ ~80 B headroom; decode/encode scratch must stay on the caller's stack, never a shared member |
| `kSyncCheckpointIntervalMs` | 5000 | `[EXPERIMENTAL]` | bounds marker write rate; power cut loses ≤ interval ⇒ idempotent re-sync |
| `kStorageTaskPriority` | 2 | `[DERIVED]` | ≤ 3 (wave budget), below real-time tasks; runtime.md says "low" |
| `kStorageTaskStackBytes` | 4096 | `[EXPERIMENTAL]` | LittleFS + scan scratch; watermark measured at Validate |
| `kStorageIdlePollMs` | 1000 | `[EXPERIMENTAL]` | pump wake-up to flush a pending checkpoint |
| `kKvKeyMaxLen` | 32 | `[EXPERIMENTAL]` | SettingSpec `name` identifiers are short |
| `kMaxCorruptResyncsPerBoot` | 32 | `[EXPERIMENTAL]` | caps boot-scan CPU; beyond → skip whole segment + counter |
| `kPartitionStorageSize` | 0xC0000 (768 KB) | `[DERIVED]` | 256 KB events + 288 KB windows (8×36 KB) + 32 KB KV + ~192 KB segment/LittleFS slack |
| (in `wire_storage.h`) task period/priority/stack above | | | single wiring-owned location for task constants |

**Task/pump:** `StorageTask`, core **0**, event-driven (blocking take with `kStorageIdlePollMs` timeout), priority 2 — per runtime.md §3.2. **Event-log writes happen inside `pump()` on this task; KV (`put`) and window (`putWindow`) writes are synchronous in the caller's task — they have no queue, so callers must be non-RT tasks (§5);** `readFrom` is read-only and may run in any non-RT caller (CommsTask). No heap in the pump path; `LittleFS.begin`/mount happens once at wiring time (startup), with the fault reporters attached first so a fallback format reports (F-03).

FIX-stage additions to existing rows: T3 also asserts the medium matches the index after a runtime repair (file size == `currentUsedBytes`, F-08); T5 and T10 also assert that the final Full refusal is *reported* (F-06).

---
