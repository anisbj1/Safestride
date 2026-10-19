# F28 — Data export, research mode & replay tooling

> Track **D** · Milestone **M1 (serial capture) → M4** · Runs on **firmware + tools** · Contract changes only via change request.

## 1. Responsibility
Make data collection and offline analysis first-class: serial capture of raw/feature streams, session files with metadata, replay of recorded sessions through the pure-logic pipeline on the host, and later export from the companion.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `ImuSample`, `PpgSample`, features, `StateSnapshot`, events, markers (via providers/queues)
- Research-mode flag (F24) — interim at M1: `kResearchCaptureDefaultEnabled` config flag, OR-ed with `OperatingMode::Research` (D4)

**Outputs (produced contracts)**
- Serial line protocol (`02` §6.4)
- `tools/capture` (Python, session directories)
- `tools/replay` (host runner of the C++ pipeline over recorded CSV)
- Event decoder for binary event records
- Export CSV/Parquet

## 4. Dependencies
- `bw_core` only; replay runner links other libs only in `tools/` build targets (allowed: it is an app, not a feature).

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Run outside Research mode unless explicitly enabled.
- Change pipeline behavior.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Transport**
- Simplest: Serial CSV.
- Professional/scalable: BLE bulk export and file-based export from the companion.
- Trade-offs: Serial is trivially reliable for the lab; BLE is needed away from a PC.
- **Choice:** Serial first (M1), companion export later (M4).

**D2. Serial bandwidth**
- Simplest: 115200 baud.
- Professional/scalable: 921600 baud.
- Trade-offs: Higher rate needs a good cable/adapter.
- **Choice:** 921600 `[DERIVED]`: 50 Hz × 6 + 100 Hz × 2 values ≈ 500 values/s ≈ 6–10 KB/s of CSV, above what 115200 (≈ 11 KB/s max, with overhead) can sustain safely.
  - Worst-case M1 load with the raw tee on: I rows 50 Hz × ~45 B ≈ 2.3 KB/s + P rows 100 Hz × ~28 B ≈ 2.8 KB/s + F/E/M ≈ 0.3 KB/s ≈ **5.4 KB/s ≈ 6 % of 92 160 B/s** `[DERIVED]`; head-room absorbs diagnostics interleaving.

**D4. Research-mode gating (F24 does not exist yet)**
- Problem: §5 — must not run outside Research mode unless explicitly enabled — but there is no F24 to set `OperatingMode::Research`.
- Simplest: compile-time config flag, default **OFF**.
- Scalable: runtime gate on `StateSnapshot::mode == OperatingMode::Research` (the enum value already exists in `bw_core`), so F24 needs no contract change — only the two wiring lines named in §13 (task created unconditionally, `sources.state` pointed at the provider).
- **Choice:** both, OR-ed: `enabled = kResearchCaptureDefaultEnabled || mode == Research`. The flag is a named constant `[USER]`, default `false`, resolved from the `F28_RESEARCH_CAPTURE_ENABLED` build flag so a capture build flips it per build without editing the repo (§15 has the exact command). Assumption documented in §13 `[ASSUMED]`: F24 absent at M1, so the flag is today's explicit enable; the mode path is wired but inert until F24 lands.

**D5. Tap map (what is read, where from, why it is safe)**
| Stream | Row | Source | Consumer safety |
|---|---|---|---|
| Motion/Ppg features | `F` | `ctx.motion_features` / `ctx.ppg_features` (`LatestValue`, read via `IFeatureProvider::latest*`) | non-destructive mailbox, multiple readers allowed; de-duped by sequence number |
| Activity / wear / state context | `F` (kind=context) | `ctx.activity_estimate` mailbox + `IStateProvider::latest` | same |
| Events | `E` | `IEventStore::readFrom(seq+1, …)` (read-only provider, bw_core) | non-consuming read; baseline-drained at stream start so old events are not re-emitted (D8) |
| User markers | `M` | emitted when a `UserMarked` event is observed | derived from the `E` tap, no new dependency |
| Raw IMU/PPG | `I` / `P` | **D3 tee** into F28-owned `SpscQueue`s | never touches `ctx.imu_samples` / `ctx.ppg_samples` |
- Explicit rule for the implementer: **the streamer must never call `pop()`/`read()` on `ctx.imu_samples` or `ctx.ppg_samples`.** A review check for this goes in the self-review list.

**D6. Separating research CSV from existing diagnostic traffic**
- Problem: `app.cpp`'s bench loop already prints `[PIPE]`/`[BTN]` (and boot text) on the same UART.
- Simplest: interleaved prints corrupt rows → discard anything unparseable.
- Scalable: strict prefix filter + keep the rest verbatim.
- **Choice:** a line is a research row **iff** it matches `^#HDR,` or `^[IPFEM],<digit|->`. Everything else is copied unchanged to `diagnostics.log` in the session directory (never dropped silently) with a count in `session.json`. Rows are newline-framed; a row mangled by mid-line interleaving fails the field-count/number check, is counted as `malformed_rows`, and is stored in `diagnostics.log` too — never "repaired". Risk noted in §13: two tasks writing to one UART can interleave *within* a line; mitigation is the counter plus the developer-side advice to keep the bench loop quiet during a capture (§15), not a lock across owners.

**D8. Event tap semantics**
- Simplest: emit every event the store returns.
- Problem: that re-emits history from previous sessions into a fresh capture.
- **Choice:** at stream enable, baseline-drain `readFrom()` without emitting (counter only), then emit only `seq > baseline`. Batch `kEventPollBatch` per tick. `[ASSUMED]` (§13): `IEventStore::readFrom` is safe to call while F18 appends — if F18 says otherwise, the fallback is an integration note (serialize through StorageTask) or dropping `E` rows for M1; `E` rows are not required by the replay round trip.

**D9. Numeric precision and replay tolerance**
- Fixed formats, written once into `DATASET_FORMAT.md`: integers plain (`t_ms` as `%llu`, raw PPG `%u`), g-values `%.4f`, dps `%.1f`, bpm `%.1f`, confidences/SQI `%.3f`, perfusion `%.5f`, freq `%.3f`. Rationale: ≥4 significant digits keeps formatting error ≤ 0.5 ulp of the last shown digit, below any pipeline threshold.
- Round-trip tolerance for tests `kFeatureToleranceRel = 1e-3` / `kFeatureToleranceAbs = 1e-4` `[EXPERIMENTAL]` (x86 vs xtensa libm + printed rounding); quality enums, `valid`/`hr_valid`/`motion_corrupted` must match **exactly**. Comparison is only meaningful when `raw_drops == 0`; the runner reports "drops present — comparison invalid" with a distinct exit code instead of a fake pass.

**D10. Session identity and boundaries**
- `session.json` schema version = `kSessionSchemaVersion` (1). `#HDR` repeats every `kHeaderRepeatMs` so a tool started mid-stream still learns schema/fw/`boot_id` within 30 s. A changed `boot_id` (reboot) or schema/fw in a new `#HDR` **starts a new session directory**; rows always carry `t_ms` + `boot_id` in the session files (boot_id comes from the header). Device clock is authoritative for `t_ms`; the host only records its own UTC in `session.json` — timestamps are never rewritten.

**D11. Producer fairness in `runTick` (F2 fixture finding, 2026-10-13)**
- Problem: the production loop gave `produceRaw()` strict priority on every pass; with the tee never empty (F1) each pass ended in `produceRaw()`, so Motion/Ppg/Activity/Events rows were never even attempted — 0 `F` rows in the 10-minute capture while `feature_missed` stayed 0 (nothing was emitted, so nothing was missed).
- Simplest: a per-tick raw cap (after N raw rows, features are tried) — but N must sit above the worst-case raw production (15 rows/100 ms) *and* below the pass budget, i.e. a second tuned budget that becomes a drop source if raw production ever grows.
- Also simple: alternate raw/feature passes — a bursty side then starves its partner for whole ticks.
- Professional/scalable: a round-robin cursor over the producers — each pass starts one past the tap that emitted; no budget to tune, and any producer added later inherits fairness for free.
- Trade-offs: round-robin costs ≤4 empty checks per pass (sub-µs) plus one `uint8_t` of state; a cap is one constant but regresses raw throughput the moment it is mis-tuned.
- **Choice:** round-robin cursor `next_tap_` (spec §7 streamer, `test_fairness.cpp`). Raw still wins every pass it is tried in under the same `kMaxLinesPerTick` budget and ring reserve ⇒ no raw regression (pinned by `test_fairness_rotation_emits_all_pending_raw_rows`); every producer is at the head of a pass within ≤ `Tap::Count` passes ⇒ no starvation (pinned by the two revert-fails tests).

**D12. `compare.py` consistency model: boolean-by-value, input coverage, recording-precision envelope, lost-row attribution (2026-10-13, bench session `research_20261013T161601Z`)**
- Problem: the §14 bench capture replayed with 2 109 field diffs (exit 3) although replay-vs-replay is bit-identical (598/598 rows). Three root causes, each proven: (1) `session.py` wrote Python `str(bool)` (`"False"`) into `features.csv`, violating `DATASET_FORMAT.md` §5 (booleans are `0`/`1`) — 1 794 diffs; (2) rows whose window inputs were never recorded (capture attached at ~1.06 s; 11 torn `I` rows = 12 gap windows) are not recomputable — 13 rows; (3) the device computes from full-precision float32 samples while the replay recomputes from `I` rows printed at `%.4f` accel / `%.1f` gyro — a K=5 print-bin perturbation probe reproduced the observed drift exactly (jerk p50 2.0e-4 / p90 6.0e-4 on both device and probe; dominant-freq threshold flips 0↔~0.5 Hz), so it is input quantization, not pipeline drift — ~276 field diffs.
- Simplest: widen `TOLERANCE_*` until the session passes — forbidden by D9 (swallows real regressions) and it fixes neither (1) nor (2).
- Also simple: require a higher-precision re-capture (`%.6f`) — models away the recording's actual precision instead of checking against it, and costs a bench run.
- Professional: make `compare.py` state what it can actually verify: booleans compare **by value** (`"True"`/`"False"`/`0`/`1` all accepted); rows with unrecorded window inputs are detected from the recorded stream (`_Coverage`, slot rule = `session.py`'s modal-delta rule, exact at the observed ±1 ms jitter — histogram {19:600, 20:28703, 21:600, 60:11}) and reported `not comparable` with counts; numeric values beyond base tolerance are accepted only inside a per-field **recording-precision envelope** `ENVELOPE_SPREAD × ENVELOPE_HEADROOM × quantum/ENVELOPE_QUANTUM_REF` (constants measured from the probe — max reachable spread covers max device drift: jerk 1.2e-3 ≥ 1.0e-3, freq 0.562 ≥ 0.532, enmo/mag_std 1e-4 = 1e-4, tilt/gyro/stillness 0 — and scale with the session's measured input quantum, so a future `%.6f` capture is verified ~100× tighter with no code change); a replay row absent from `features.csv` is fatal unless torn research fragments in `diagnostics.log` prove recording-side loss (then reported `lost in recording`, counted).
- Trade-offs: baked constants must be re-measured if the print format changes (but the quantum scaling handles precision changes automatically); per-session Monte-Carlo re-runs re-derive the envelope but converge badly at the tails (74/276 rows fell outside a 5-run envelope); widened tolerance needs no measurement but violates D9. Coverage rules trust modal-delta slot arithmetic — proven exact on this capture's jitter histogram.
- **Choice:** the professional option; `TOLERANCE_*` untouched (D9 intact). Every exclusion, acceptance and loss is printed and counted in the verdict line (never silent — same honesty rule as `raw_drops` ⇒ exit 2), and each part is pinned by a revert-must-fail test (H10): bool mapping, head/gap coverage, envelope accept, torn-evidence attribution, and `session.py`'s `_csv_cell`.

## 7. Module structure
**Firmware (`bw_research` compiles `bw_core` only — no F02/F01 headers):**
```
firmware/lib/bw_research/
  library.json
  include/bw/research/research_config.h   // every constant below, tagged; the enable build-flag
  include/bw/research/line_format.h  + src/line_format.cpp    // pure: value structs -> one line into a caller buffer
  include/bw/research/raw_tee.h      + src/raw_tee.cpp        // F28-owned SpscQueues; onImu/onPpg (gated), drop counters
  include/bw/research/research_streamer.h + src/research_streamer.cpp
                                          // pure step(): providers/mailboxes in -> one line out; gating, de-dup,
                                          //   event baseline-drain, drop/malformed counters
firmware/src/app/wiring/wire_research.{h,cpp}   // f28::wire(AppContext&), ResearchTask fn + its stack/prio/period
firmware/test/bw_research/test_bw_research/     // Unity suites (mirrors the bw_activity layout)
```
`wire_research.cpp` constructs the streamer, wires provider/mailbox pointers from `AppContext`, creates `ResearchTask` (core 0, low priority — UART drain is never on the real-time core), and exposes the `rawTee()` accessor the producer-side one-liner needs (§15). No logic in wiring: the task function is a thin wrapper — read the mailboxes/providers, call the pure `bw_research::ResearchStreamer::runTick()`, write produced lines with non-blocking `Serial` calls — so the decision-making code is exactly what the host tests exercise.

**Host tools:**
```
tools/capture/            Python package: cli.py (argparse), transport.py (serial | file),
                           lines.py (parse/validate, prefix split), session.py (session dirs)
tools/capture/tests/      pytest + committed fixtures
tools/replay/             standalone native PlatformIO project (D7): src/main.cpp,
                           include/replay/session_reader.*, compare.py (tolerance gate),
                           tests/ (pytest, fixture session)
tools/decode_events.py    Event 48-byte decoder (layout pinned by DATASET_FORMAT test vector)
tools/export/             export half — planned in §16 (W5); no production code before that plan's IMPLEMENT
docs/contracts/DATASET_FORMAT.md  // stays STUB -> Draft at IMPLEMENT; exact grammar + test vectors
```
**M4 forward sketch (out of scope now):** the companion app exports the same session directory content over BLE using the existing chunked `Sync Data` characteristic pattern (F20 cursor-and-ack), producing an identical `session.json` + CSV set on the phone; `session.json`'s `kSessionSchemaVersion` and the line format version already give the two transports one schema, so `tools/export` (Parquet/CSV) can consume either source without change. Nothing in M1–M3 may depend on BLE (§5 of `00`).

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kSerialBaud` | 921600 | `DERIVED` | See D2 |
| `kSessionSchemaVersion` | 1 | `DERIVED` | Dataset format version in `DATASET_FORMAT.md` |
| `kResearchCaptureDefaultEnabled` | false | `USER` | §5 gate while F24 is absent (D4); the explicit-enable knob for a capture session — resolves from the `F28_RESEARCH_CAPTURE_ENABLED` build flag (0/1, default 0 ⇒ false), flipped per capture build via `PLATFORMIO_BUILD_FLAGS` without editing the repo (§15 A3); `static_assert`ed binary |
| `kResearchTaskPeriodMs` | 10 | `DERIVED` | Task tick = poll + format + UART drain. Two requirements (F1 root cause): (1) drain capacity = `kDrainChunkBytes`/tick (hardware ceiling, see `kDrainBytesPerSecond`) — at the original 100 ms the 1280 B/s ceiling sat below the 2275 B/s I-row production and the tee overflowed (12 201 raw drops); (2) poll ≥10× faster than the 1000 ms feature rate (`[EXPERIMENTAL]` per 02 §3.3) — 10 ms is 100× |
| `kDrainBytesPerSecond` | 12 800 | `DERIVED` | `kDrainChunkBytes × 1000 / kResearchTaskPeriodMs`; `static_assert`ed ≥ `kWorstCaseStreamBytesPerSecond` so a period regression fails every build |
| `kWorstCaseStreamBytesPerSecond` | 5400 | `DERIVED` | D2 worst-case M1 stream (I 50 Hz + P 100 Hz + F/E/M ≈ 5.4 KB/s); drain must sustain it or ring → tee overflow repeats |
| `kMaxLinesPerTick` | 40 | `DERIVED` | Caps per-tick formatting work (~0.15 ms) so the research task can never eat its own period |
| `kLineRingBytes` | 1024 | `DERIVED` | Holds one full tick of worst-case output (~750 B) if the UART stalls, ≈7 longest lines |
| `kMaxLineLen` | 256 | `DERIVED` | Longest legal row is an E line ≈ 220 B (TimeMs + 16-hex payload + longest enum names + 16 separators); +headroom, pinned by `test_event_worst_case_line_fits_max_line_len` |
| `kImuTeeDepth` | 16 samples | `DERIVED` | 320 ms of 50 Hz ≫ 10 ms poll; 512 B static |
| `kPpgTeeDepth` | 32 samples | `DERIVED` | 320 ms of 100 Hz; 768 B static |
| `kHeaderRepeatMs` | 30 000 | `EXPERIMENTAL` | D10: a capture tool started mid-stream learns metadata within 30 s |
| `kEventPollBatch` | 8 | `DERIVED` | Event bursts (boot/fault storms) drain in ≤2 ticks |
| `kResearchTaskStackBytes` | 4096 | `EXPERIMENTAL` | `snprintf` float formatting + line buffer, no recursion, measured at DoD |
| `kResearchTaskPriority` | 2 | `DERIVED` | Below every pipeline task (SensorTask 5, ProcessingTask 4, F01 D15): capture is diagnostics and must never delay the pipeline |
| `kResearchTaskCore` | 0 | `DERIVED` | Core 0 hosts UART/BLE/flash; keeps capture off the core-1 sensor/processing path |
| `kLineRingReserveBytes` | 256 | `DERIVED` | Production pauses when fewer than one max line remains, so an append never partially fits |
| `kDrainChunkBytes` | 128 | `DERIVED` | One bounded UART write per drain (~1.4 ms at 921600), never blocking |
| `kMaxFirmwareVersionLen` | 32 | `DERIVED` | `#HDR` field stability: longer versions truncated after sanitising `,`/newlines |
| `kEventBaselineMaxEvents` | 512 | `DERIVED` | D8 activation scan cap: a pathologically full store cannot stall activation; larger stores are a §13 assumption |
| `kUnknownBootId` | 0 | `DERIVED` | No boot_id source in AppContext yet (F17 not integrated): 0 = "unknown" |
| `kFeatureToleranceRel` / `kFeatureToleranceAbs` | 1e-3 / 1e-4 | `EXPERIMENTAL` | D9: x86-vs-xtensa libm + printed rounding; tightened if the fixture shows less drift |
| `ENVELOPE_SPREAD` / `ENVELOPE_HEADROOM` / `ENVELOPE_QUANTUM_REF` (in `compare.py`) | per-field (enmo/mag_std 1e-4, jerk 1.2e-3, dominant_freq 0.562, tilt/gyro/stillness 0) / 2 / 1e-4 | `EXPERIMENTAL` | D12: max feature drift reachable when the replay recomputes from inputs rounded to the print quantum (K=5 print-bin probe on the 2026-10-13 bench capture), ×2 headroom, scaled by the session's measured input quantum; `compare.py` only — `kFeatureTolerance*` (D9) untouched |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
Host only — no hardware needed. Suites live in `firmware/test/bw_research/test_bw_research/` (`pio test -e native`) and `tools/capture/tests`, `tools/replay/tests` (`pytest`); **0 tests removed or loosened** (baseline before this plan: 452 native cases / 9 suites, all passing).

**Python (pytest, ruff clean, fixtures committed):**
7. `test_lines.py` — every golden line parses; wrong field count / non-numeric ⇒ counted as malformed, never crashes; **prefix split** (D6): `[PIPE]`, `[BTN]`, boot text, mid-line interleaving garbage ⇒ `diagnostics.log`, research rows untouched; partial line at EOF.
8. `test_session.py` — session dir layout + `session.json` with `kSessionSchemaVersion`; every row carries `t_ms` + `boot_id`; `boot_id`/schema change ⇒ new session dir; clean finalize on Ctrl-C and on disconnect (files closed, `end_reason` written).
9. `test_decode_events.py` — decoder on the shared 48-byte vector ⇒ field-for-field match; the vector is read **from `DATASET_FORMAT.md`** so doc↔decoder drift fails the test (H6/H9).
10. `test_replay.py` (`tools/replay/tests/`) — committed fixture session (synthetic raw ⇒ `I`/`P` lines) → parser → session files → `tools/replay` runner → recomputed `MotionFeatures`/`PpgFeatures` equal the fixture's expected features within `kFeatureTolerance*`, enums/validity exact; drops-present fixture ⇒ distinct "comparison invalid" exit code (never a pass); malformed-row fixture ⇒ counted, session still finalized.
11. **Round-trip at the line level (the §9 requirement):** device line format → capture parser → session files → replay → features within documented tolerance — covered by 7+8+10 as one chain, plus the firmware side (1+2+5) so the device formatter and host parser are pinned to the same goldens.

12. `test_fairness.cpp` (firmware; D11) — full tee backlog before **every** tick ⇒ motion + context rows still emitted in the same tick (revert to strict priority ⇒ 0 rows, both tests fail — H10); one device tick's 5 IMU + 10 PPG + pending motion ⇒ every row emitted, tee and line drops 0 (round-robin must not cost raw anything); 10 ticks of sustained backlog ⇒ exactly one motion row per tick (cursor state is correct across ticks).

**Deliberately not covered here:** real UART behavior at 921600 (needs hardware — §15 developer action), F24 mode entry, BLE export (M4).

## 10. Failure behavior
- Serial back-pressure ⇒ drop rows with a counter rather than stalling tasks.

## 11. Budget
RAM < 4 KB; runs only in Research mode.

Static (M1, all resident even when OFF — static-only rule, accepted simplicity):
| Item | Bytes |
|---|---|
| line ring `kLineRingBytes` | 1024 |
| IMU tee `kImuTeeDepth` × 32 B | 512 |
| PPG tee `kPpgTeeDepth` × 24 B | 768 |
| line buffers (in + out) + streamer state | ~450 |
| **total** | **≈2.7 KB < 4 KB** |

CPU: ~155 rows/s × ≈3 µs formatting ≈ **0.5 ms/s ≈ 0.05 %** `[DERIVED]`, core 0, low priority; UART drain uses `availableForWrite()` and never blocks. Task stack 4 KB (§8). Measured at DoD (H3).

## 12. Out of scope
- Cloud upload.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- A 10-min capture and replay reproduces features (recorded in the status log).

---

## 15. M1 implementation plan (PLAN stage, 2026-10-11 — no production code yet)

Scope = the M1 exit: device `ResearchStreamer` + `wire_research.*`, `tools/capture`, `tools/replay`, `tools/decode_events.py`, `DATASET_FORMAT.md`. Companion/BLE export stays M4 (§7 sketch). Nothing here changes pipeline behavior (§5): every tap is a read-only provider/mailbox read, and the only producer-side addition is the gated tee call (D3).

**Rollback / failure posture:** flag OFF (repo default) ⇒ byte-for-byte prior behavior — no task, no writes, no UART change; tee push gated by the same flag; any formatting overflow ⇒ row dropped + counted (§10), never a stall; capture-side parse failures ⇒ counted + preserved in `diagnostics.log`, session still finalized.

---

## 16. W5 export-half implementation plan (PLAN stage, 2026-10-17 — no production code)

Scope = the remaining §3 output **"Export CSV/Parquet"**: a host tool `tools/export` that turns **recorded F28 session directories** (`tools/capture` output) plus the **binary event decode path** (`tools/decode_events.py`, CR-012 canonical 48-byte vector in `DATASET_FORMAT.md` §5) into schema-annotated CSV datasets.

**Documented follow-up (not this wave):** export from the companion's **SQLite DB** (F21). F21's W5 app schema is being planned in parallel — nothing here imports, reads or assumes it (D14 leaves a source-agnostic seam instead). Parquet is deferred under D16 with an explicit trigger.

### Decisions

**D14. Export sources (W5)**
- Problem: §3 says "Export CSV/Parquet"; candidate sources are recorded session dirs (on disk, schema-pinned) and the companion SQLite DB (F21, schema in flux).
- Simplest: session dirs only — committed fixtures exist, `session.json` v1 is the pinned schema, no other feature in the loop.
- Professional/scalable: a pluggable source seam (`session dir | SQLite | BLE-received session`) feeding one writer.
- Trade-offs: consuming F21's DB now couples F28 to a schema designed in parallel (integration deadlock; forbidden dependency); a formal `ABC` source interface with only one implementation is an abstraction without a second client.
- **Choice:** session dirs now; `cli.py` → `session.py` (validate) → writer passes an in-memory `Session` (tables + metadata dicts), so the writer never touches paths/DB internals — the SQLite reader is a later drop-in that produces the same `Session`. SQLite export recorded as a follow-up in the status log.

**D15. Output shape**
- Problem: "dataset" could mean one joined ML table, or annotated per-source tables.
- Simplest: one CSV per source, rows byte-preserved, `#META` schema rows on top + `export_manifest.json` (session metadata, counters, file list, absent-file flags).
- Professional/scalable: a single joined, resampled `dataset.csv` on a common time grid.
- Trade-offs: a join/resample picks alignment and interpolation rules ⇒ **silent transforms**, forbidden by the tooling rule ("never change timestamps silently; keep raw as recorded"); 50/100 Hz raw vs 1 Hz features has no lossless join, and F05/F06/F29 each align differently — one baked grid would encode one researcher's choice in the export layer.
- **Choice:** per-source CSVs, verbatim data rows + `#META` rows + manifest. Alignment/resampling stays visible in `ml/` consumer code.

**D16. CSV vs Parquet**
- Simplest: CSV only — stdlib `csv`, zero dependencies; a 10-min session is ~90 k rows (30 k IMU + 60 k PPG), trivial for pandas/R.
- Professional/scalable: Parquet — typed columns, compression, embedded schema, ML-standard.
- Trade-offs: Parquet needs `pyarrow` (heavy binary wheel; **not dependency-free** — `import pyarrow` → `ModuleNotFoundError` in this environment) plus the D13 mirror rule (both `pyproject.toml`s + env installs), and would either put a binary wheel on the pytest floor or need `importorskip` gating; CSV carries types via the `#META` rows instead (D15) at zero cost.
- **Choice:** **CSV only** in W5. Parquet = documented follow-up: implement behind a lazy `pyarrow` import (clear install-hint error, `pytest.importorskip` tests) **iff `ml/` asks for columnar** — then D13's mirror update rides along. No `--format` flag ships until a second format exists (no flag that can only fail).

**D17. Lossy sessions**
- Problem: captures can carry `raw_drops>0`/`malformed_rows>0` (the 2026-10-11 bench fixture did); `compare.py` refuses to bless them (exit 2).
- Simplest: always export — the dataset then silently looks complete. Also simple: always refuse — blocks legitimate debugging exports.
- **Choice:** refuse by default (**exit 2**, the same exit-code map as `compare.py`), `--include-lossy` lifts the gate, and all counters (`raw_drops`, per-source split, `malformed_rows`, `drop_history` age) go into `#META` rows either way. Never repair or drop rows silently (§10).

**D18. Binary event path**
- Problem: events reach export in two shapes: text `events.csv` (capture) and packed 48-byte `Event` records (sync/BLE/SQLite later).
- Simplest: parse `events.csv` only — the binary path stays untested until F21. Scalable: one adapter with two readers normalizing to the same row dict, layout owned solely by `decode_events.py` (CR-012 vector).
- Trade-offs: the adapter is a few lines + one test; re-stating the 48-byte layout anywhere else = a second source of drift (forbidden).
- **Choice:** `events_source.py` with `read_events_csv` / `read_events_bin`, both delegating to `tools/decode_events.py`; pinned by the cross-check test (vector bytes → decoded fields == the §5 golden `E,...` row's fields).

### Module structure (host only, stdlib only)

```
tools/export/
  __init__.py
  cli.py            # argparse: <session_dir>... --out DIR --include-lossy; exit-code map
  session.py        # read + validate session.json/CSVs (schema_version, columns, enums, 0/1 bools) -> Session
  metadata.py       # #META row model: column defs (name/unit/type/source) per DATASET_FORMAT §3
  csv_export.py     # writer: #META rows + manifest + source rows verbatim
  events_source.py  # events.csv reader + 48-byte binary reader (delegates to decode_events.py)
  tests/            # conftest.py sys.path insert, mirrored from tools/capture/tests/conftest.py
```

Imports: `csv`/`json`/`argparse`/`pathlib` + `decode_events` (same `sys.path` mechanism `capture` uses). **No** pyarrow, pandas, pyserial.

### Constants (named, tagged, justified)

| Name | Value | Tag | Why |
|---|---|---|---|
| `DATASET_FORMAT_VERSION_SUPPORTED` | 1 | `DERIVED` | Must equal `session.json`/`#HDR` `schema_version`; unknown ⇒ refuse, exit 4 with the version printed (`DATASET_FORMAT.md` §4, same rule as `compare.py`) |
| `EXPORT_SCHEMA_VERSION` | 1 | `DERIVED` | Version of the export envelope (`#META` rows + manifest), independent of the dataset `schema_version`; bump = additive metadata change only |
| `META_ROW_PREFIX` | `"#META"` | `DERIVED` | Starts with `#` so `read_csv(comment="#")` skips metadata, and stays distinct from the device's `#HDR` so an exported file can never be misread as a capture line (host side of the D6 prefix rule) |
| `DEFAULT_INCLUDE_LOSSY` | `False` | `DERIVED` | D17 default gate — loss is recorded in `#META`, never silent; mirrors `compare.py` exit 2 |
| `EVENT_RECORD_BYTES` | 48 | `DERIVED` | CR-002/CR-011 `Event` layout, canonical vector CR-012 (`DATASET_FORMAT.md` §5); asserted **equal to `decode_events`' record size at test time** — export never restates the layout |
| `EXIT_OK` / `EXIT_REFUSED_LOSSY` / `EXIT_BAD_SESSION` | 0 / 2 / 4 | `DERIVED` | Same exit-code map as `compare.py` (2 = lossy recording, 4 = missing session/unknown schema); 3 stays compare-only (field mismatch), export never claims a comparison |

### Test list (pytest, `tools/export/tests/`; floor: `python -m pytest tools` = **73 collected** at W5 base → 73 + N, **0 removed**)

1. `test_metadata_rows.py` — every exported file starts with `#META` rows whose column definitions **equal** `DATASET_FORMAT.md` §3's per-file column lists (doc parsed the way `test_decode_events.py` parses §5 — H9 drift guard, both directions: a doc column dropped from the export fails, an exported column missing from the doc fails); `META_ROW_PREFIX` starts with `#` (comment-skip claim pinned).
2. `test_export_csv.py` — committed fixture session → export ⇒ data rows byte-equal to source (timestamps untouched, booleans still `0`/`1`, no transformation); mutated `schema_version: 2` ⇒ exit 4, version printed; mutated `raw_drops>0` ⇒ exit 2 by default and `--include-lossy` ⇒ exports with counters in `#META`; `raw_tee:false` session without `raw_ppg.csv` ⇒ absence recorded in the manifest, **never a fabricated empty file**; `markers.csv` `source=device|host` preserved; `labels.csv` exported iff present; truncated source CSV ⇒ error **before** any output file is written (validate-then-write).
3. `test_events_bin.py` — parse the `F28-EVENT-VECTOR-48B` block from `DATASET_FORMAT.md` ⇒ `read_events_bin` fields == the §5 golden `E,...` row's parsed fields (doc ↔ `decode_events` ↔ export in one chain; editing the vector, the decoder or the export adapter alone fails it); truncated record (< 48 B) ⇒ refused, no partial row; multi-record input keeps input order (no re-sort, no repair); `EVENT_RECORD_BYTES == decode_events`' record size (imported, not redefined).
4. `test_export_cli.py` — happy path exit 0; `--include-lossy` flag; refusal exit codes 2/4; existing non-empty `--out` target refused (never overwritten); `--help` clean; `python -m ruff check tools` clean after every commit.

**CR-022 (draft content — raise at IMPLEMENT, never apply):** additive `DATASET_FORMAT.md` §7 "Export output (host, `tools/export`)" = `#META` row grammar (`#META,<kind>,…`, kinds `session` / `column,<idx>,<name>,<unit>,<type>`), `export_manifest.json` field list, exit-code map, `EXPORT_SCHEMA_VERSION` semantics; §6 version-history row; `schema_version` stays **1**, §5 vectors untouched (append-only, 02 §6.6).

**Rollback posture:** host-only tool; capture/replay behavior untouched; zero new dependencies (D13 mirror files unedited); source session dirs opened read-only, output to a separate directory.

**Follow-ups (recorded, not W5):** (a) F21 SQLite source adapter (needs F21 schema frozen); (b) Parquet writer behind optional `pyarrow` (D16 trigger: `ml/` asks); (c) `labels.csv` reviewer workflow.

---
