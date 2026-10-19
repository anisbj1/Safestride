# F22 — Explainable event information

> Track **C** · Milestone **M3 (codes) → M5** · Runs on **firmware + companion** · Contract changes only via change request.

## 1. Responsibility
Human-readable explanations for important detections: compact reason codes on the device, full text and contribution ranking on the companion and (short strings) on the OLED.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `ReasonMask` in `StateSnapshot`/`Event`
- `IModel::contributions` (optional) via reason-code mapping done by F06/F09

**Outputs (produced contracts)**
- `explainShort(ReasonMask, char* buf, size_t)` for OLED
- Reason text catalog (versioned, shared with companion)
- Ranked contributor list for the companion

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Change decisions.
- State certainty or diagnosis.
- Produce text for unknown codes silently (must show 'code N').

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Where text lives**
- Simplest: Hard-coded strings in firmware.
- Professional/scalable: Firmware carries codes + short strings; companion carries full catalog by code.
- Trade-offs: Catalog version must match.
- **Choice:** Codes on device, catalog on companion, short OLED strings on device.

**D2. How catalog completeness is enforced (§9, plan stage 2026-10-12)**
- Problem: the test must fail when a `ReasonCode` is appended to `bw_core` without text, and it must be pinned to the real enum, not to a stale hand-written copy.
- Simplest: a hand-maintained name list inside the test — but it goes stale exactly when a code is added, so the test passes when it must fail.
- Scalable: a `Count` sentinel in the enum — compile-time, but costs a contract CR into `bw_core` (CR-017, reserved) for something avoidable.
- Chosen: the native test parses the `enum class ReasonCode` block out of `firmware/lib/bw_core/include/bw/state.h` at run time (enumerator identifiers in source order = numeric values, enum is append-only) and asserts: parsed count == catalog entry count, every index has non-empty text ≤ `kMaxShortReasonChars`, and every enumerator name appears symbolically as `ReasonCode::<name>` in `bw_explain` sources (no numeric casts). Appending a code without text ⇒ test fails. If `state.h` cannot be located, the test FAILS loudly (never skips); candidate relative paths tried in order (constant `kReasonHeaderCandidates`). Guarded additionally by `static_assert`s pinning each current enumerator's numeric value (renumber/reorder guard). No CR needed.

**D3. `explainShort` return semantics**
- Alternatives: return bytes written (simplest, caller cannot detect truncation); `Result<size_t>` (heavy for string formatting); snprintf-style: return would-be length excluding NUL (caller detects truncation by comparing with capacity, zero API cost). Cap==0 writes nothing and still returns the would-be length; buffer is NUL-terminated whenever cap>0.
- Choice: snprintf-style. Always writes into the caller's buffer only — no static/shared buffer, no heap.

**D4. Ordering, selection and empty mask in `explainShort`**
- Selection when more than `kMaxShortReasons` bits are set: ascending bit index (deterministic, stable, testable) vs a per-code importance table (duplicates alert policy inside F22, wrong owner). Choice: ascending bit index; the caller computes omitted count via `popcount(mask)` and may show "+N" (F12's concern, not F22's).
- Unknown bit renders inline at its own position as `reason #N` (§10), never silently skipped, never at the end (position carries meaning).
- `mask == 0` ⇒ empty string, return 0. "No reasons" placeholder text is the caller's (OLED width/language) decision, not F22's.

**D5. Ranked contributor list (§3 output) — deferred to W5**
- The companion has no consumer until F21 scaffolds the app (`docs/05` wave table: F21 skeleton → companion halves incl. F22). Firmware has no consumer for ranking this wave (F12/F14/F17 take reason masks/short strings only; `IModel::contributions` mapping is F06/F09's).
- Choice: implement `explainShort` + versioned catalog now; add `rankContributions()` (sort by `|weighted_value|` desc, host-tested) together with the companion `ExplanationBuilder` in W5. Deferral recorded in status log.

**D10. Exact-seq lookup across devices (audit F22-AUD-1, fixed 2026-10-17)**
- Problem: `for_seq(seq)` without a device scope matched on **seq alone**, so the same seq on two devices returned whichever row `timeline()` ranked first — silently, while the API promises a *per-device* seq.
- Simplest: keep returning the first row (fast, but ambiguous and dishonest). Simplest honest: return `None` when a duplicate is seen (hides that the event exists at all).
- Scalable: match on `(device, seq)` — a scope (per call or per view) resolves the row and names it via `device_id`; with no scope the duplicate (PK `(device_id, seq)` ⇒ a duplicate can only be a second device) is detected and **every** candidate row is surfaced as `AmbiguousSeq`.
- Trade-offs: one extra `timeline()` page (16 rows) per lookup; candidate **device ids** are not observable through F21's frozen read seam (`EventRecord` carries no `device_id`), so ambiguity is surfaced as *rows*, and a device is *named* only when a scope was given — adding the field would be a CR into F21's contract, not justified for this prototype.
- **Choice:** the scalable option, `EXPLAIN_SEQ_LOOKUP_LIMIT = 16` `[DERIVED]`; no CR, no contract change; the CLI renders the ambiguity and exits 1 (§10).
- **W5 resolution (2026-10-17, companion half): see D8** — the ranking stays deferred even now, because the companion's only data source (the synced 48-B events) carries no contribution values. Reasoned explicitly below; CR-025 stays unused.

## 7. Module structure
Wave-3 (this feature's firmware half):
```
firmware/lib/bw_explain/
  include/bw/explain/reason_catalog.h   // reasonText(ReasonCode) -> const char* (nullptr = no entry),
                                        // kReasonCatalogVersion, kReasonCatalogCount
  include/bw/explain/explain_short.h    // explainShort(ReasonMask, char* buf, size_t cap) -> size_t
  src/reason_catalog.cpp                // {ReasonCode, text} entry table, .rodata only
  src/explain_short.cpp                 // bit walk 0..31, join with ", ", unknown => "reason #N"
firmware/src/app/wiring/wire_explain.{h,cpp}  // stateless seam (app::WireExplain), no tasks, no state;
                                              // consumers F12/F14/F17 call through it later; nothing is
                                              // wired into app.h/app.cpp this wave (integrator's step)
firmware/test/bw_explain/test_bw_explain/
  test_reason_catalog.cpp               // completeness (D2), wording, version, static_assert value pins
  test_explain_short.cpp                // formatting, ordering, unknown, truncation, boundaries
  test_wire_explain_compile.cpp         // seam compiles for native + pass-through equals library
                                        // (precedent: bw_ui/test_wire_ui_compile.cpp)
```
`reasonText` returns `nullptr` for an out-of-catalog code instead of a placeholder, because formatting `reason #N` needs the caller's buffer — `explainShort` does that rendering (D4).

Deferred (unchanged from W3; explicitly re-checked in W5 — see D8):
- `rankContributions()` (companion or firmware): no contribution data exists in the synced DB and
  `firmware/lib/bw_explain/**` is DONE/FROZEN this wave; CR-025 reserved, unused.

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kMaxShortReasons` | 3 | `EXPERIMENTAL` | OLED space (existing) |
| `kReasonCatalogVersion` | 1 | `DERIVED` | schema-ish version for the shared catalog: bump on any code↔text change so the companion (W5) can detect a mismatch (core §1 rule: transmitted/persisted value gets a version) |
| `kMaxShortReasonChars` | 14 | `DERIVED` | longest entry bound: worst-case unknown `reason #31` = 10 chars + margin for wording edits; enforced by test, not by convention |
| `kExplainShortBufSize` | 48 | `DERIVED` | worst case = 3·14 + 2 separators·2 chars + NUL = 47 → round up; callers may pass smaller buffers (truncation-safe, D3) |
| `kReasonHeaderCandidates` | `{"firmware/lib/bw_core/include/bw/state.h", "lib/bw_core/include/bw/state.h", "../lib/bw_core/include/bw/state.h"}` | `DERIVED` | completeness test locates the enum source without a `platformio.ini` build flag (that file is shared, not owned by F22); first hit wins, none ⇒ FAIL |
| `kReasonCodeBitWidth` | 32 | `DERIVED` | `ReasonMask = uint32_t`: `explainShort` walks bits 0..31; bits ≥ catalog size are the "unknown" path and stay testable today |

Companion half (W5):
| Name | Value | Tag | Why |
|---|---|---|---|
| `REASON_CATALOG_VERSION` | 1 | `DERIVED` | Python twin of `kReasonCatalogVersion`: bumped on any code↔text change; the parity test pins it to the firmware value parsed from `reason_catalog.h` (D9) |
| `EXPLAIN_MAX_REASON_BITS` | 32 | `DERIVED` | `ReasonMask = uint32_t` (data_decision.md §4.5): the builder walks bits 0..31; bits ≥ catalog size render `reason #N` — unknown stays testable today, exactly like the firmware half (D4) |
| `EXPLAIN_DEFAULT_LIMIT` | 10 | `USER` | CLI `--limit` default for the recent-explanations list; 10 full explanations is one readable console page (each row is multi-line), overridable per call |
| `EXPLAIN_SEQ_LOOKUP_LIMIT` | 16 | `DERIVED` | Rows one exact-seq lookup inspects (F22-AUD-1): >= 2 detects that another device recorded the same seq (PK `device_id, seq` makes a duplicate a second device); 16 bounds the candidate list an ambiguity surfaces — a lookup never picks among candidates |
| `EXPLAIN_MAX_SENTENCE_CHARS` | 120 | `DERIVED` | Per-reason sentence bound: one console line with the name prefix on typical terminals; enforced by the parity test, not by convention |
| `REASON_HEADER_CANDIDATES` | `("../../../firmware/lib/bw_core/include/bw/state.h", "firmware/lib/bw_core/include/bw/state.h", "../firmware/lib/bw_core/include/bw/state.h")` | `DERIVED` | Parity test (D6/D9) locates `bw/state.h` and `bw/explain/reason_catalog.h` relative to the test file and the repo root without touching F21's `pyproject`; first hit wins, none ⇒ FAIL (D2 discipline: a skipped check is a failed check) |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
Host only: `pio test -e native`, suite `firmware/test/bw_explain/test_bw_explain/` (plan: 3 files → implemented 4: the plan made the counting-`new` hook conditional on bw_testkit support; bw_testkit has none, so the suite-local pattern of `bw_ui`/`bw_ml` `test_no_heap.cpp` is used). Implemented: 19 tests.

**`test_reason_catalog.cpp` (spec-9 completeness, pinned to the enum — D2)**
1. Parse `enum class ReasonCode` block from `state.h` (candidate paths, fail-loud if absent) → N enumerator names in source order.
2. Assert N == catalog entry count and every index `0..N-1` has non-empty text of length ≤ `kMaxShortReasonChars` — **appending a code without text fails this test**.
3. Assert every parsed name appears symbolically as `ReasonCode::<name>` in `bw_explain` sources (no numeric-cast entries; keeps the table pinned to the enum).
4. `static_assert` numeric value of each current enumerator (append-only guard: renumber/reorder fails at compile time; a plain append does not, and is caught by 1–3).
5. Wording: no banned substrings (`diagnos`, `narcolep`, `disease`, `certainty`), ASCII printable only, no uppercase start — device never diagnoses (docs/00_PROJECT_CONTEXT.md), F12 lays out text.
6. `kReasonCatalogVersion == 1`.

**`test_explain_short.cpp`**
7. Empty mask → `""`, return 0 (first call / empty state).
8. Single known bit → its text; every known code round-trips bit→text.
9. Three bits → `"a, b, c"` in ascending bit order.
10. ≥4 bits set → exactly first 3 by ascending bit index; 4th+ dropped (D4).
11. Unknown bits (18..31, no enumerator exists yet ⇒ safe synthetic unknowns) → `reason #N` at the correct position; mixed known+unknown ordering.
12. Truncation safety: cap = 0 (no write, returns would-be length), 1, exact-fit, exact-fit−1, `kExplainShortBufSize`, huge → always NUL-terminated when cap>0, never writes past cap, snprintf-style return equals untruncated length (D3).
13. Worst case: all 32 bits set → 3 reasons + `reason #N` handled; output fits `kExplainShortBufSize` (runtime assert; formula also `static_assert`ed in the header).
14. No heap / no platform calls in `bw_explain` (source review + counting-`new` hook if `bw_testkit` offers one; precedent `bw_ui/test_no_heap.cpp`).

**`test_wire_explain_compile.cpp`**
15. `app::WireExplain` compiles for native and returns byte-identical output to the library call for the cases in 8–11 (pass-through seam, no logic of its own).

Deliberately not covered here: F12/F14/F17 rendering (their suites), companion catalog sync (W5), on-device flash measurement (Validate stage, recorded in status log).

16. **Fault-injection for the check itself** (`test_catalog_completeness_detects_appended_code`): a doctored IN-MEMORY copy of the enum block gains one enumerator exactly as a future append-only CR would — the parser must see N+1 names, so the equality in item 1 fails until the catalog gains the entry; the same test proves `reasonText` = `nullptr` and `explainShort` = `reason #18` for that future code (§10). No `bw_core` file is edited.

**Companion half (W5), suite `companion/lib/features/explain/tests/` — planned 3 files → ~15 tests.** Gates: `python -m pytest lib/features/explain/tests -q` and `python -m ruff check companion` (from `companion/`; F21's `companion/tests/` untouched).

**`test_catalog_parity.py` (D6 + D9 — the companion's counterpart of item 16's guarantee)**
17. Parse `enum class ReasonCode` from the `REASON_HEADER_CANDIDATES` paths (same regex block-parse as the firmware test, fail-loud named message when absent) → N names in source order; assert `N == len(REASON_EXPLANATIONS)`, keys == `range(N)` — appending a firmware code without a companion sentence fails here.
18. Assert parsed names == `wording.REASON_NAMES` (F21's list stays in lockstep with the contract in one assertion).
19. Parse `kReasonCatalogVersion` from `bw/explain/reason_catalog.h`; assert `== REASON_CATALOG_VERSION` (D9).
20. Catalog wording: every sentence non-empty, ≤ `EXPLAIN_MAX_SENTENCE_CHARS`, ASCII printable, no term from `wording.BANNED_DIAGNOSTIC_TERMS` (device never diagnoses).
21. **Fault-injection** (companion twin of item 16): a doctored in-memory enum block with one appended enumerator makes the count equality fail; no repo file is edited.

**`test_builder.py` (pure `build_explanation`, no DB)**
22. Single known bit → one `ReasonItem`, name + sentence; every catalog code round-trips bit → text.
23. Multi-bit mask → items in ascending bit index (D4/D8 ordering), all set bits shown (no 3-reason cap on the companion, D1).
24. Unknown bits 18..31 → `reason #N` items at their own positions; mixed known + unknown ordering.
25. `mask == 0` → `reasons == ()` (rendered "none" downstream); nothing is back-filled.
26. Detection types 0/1/2 → label from `wording.detection_label` incl. `(confidence 0.XX)`; non-detection → plain `EVENT_TYPE_NAMES` entry; confidence always rendered as a value, never as certainty.
27. Unsupported schema → `schema_supported == False`, no semantic fields rendered (F21 D5 behavior preserved).
28. Banned-terms sweep over the fully rendered output (label + all reason lines) for every `EventType` × a synthetic all-bits mask — the same safety net F21 applies to rendered text.

**`test_viewmodel_cli.py` (`ExplanationViewModel` + `main(argv)` over a seeded DB)**
29. Seed via `encode_event48` → `EventRepository.upsert` (real pipeline, contract bytes): `--seq` finds the row and explains it; unknown `--seq` → non-zero exit + named message, no traceback.
30. `--limit` newest-first list; `--device` filter (mirrors `EventRepository.timeline` semantics).
31. Default DB path honors a monkeypatched `ANAROLEPSY_COMPANION_DB` (same env contract as `app.py`); `--db` overrides.
32. **F22-AUD-1 (reverting-fails):** seq 3 seeded on devices A and B with tellable-apart content, no device scope → `for_seq(3)` returns `AmbiguousSeq` carrying BOTH candidates (never a silently picked row) and `render_ambiguous` renders both + "no row picked"; with a device scope (`for_seq(3, device_id=B)` or the view's `device_id`) → that device's row with `device_id` named in the result.
33. CLI twin: ambiguous `--seq` without `--device` → exit 1 with every candidate rendered; the same lookup with `--device` → exit 0 with only that device's row (`device ...` in the head).

Deliberately not covered here: firmware behavior (W3 suites), contribution ranking (D8 — no data), UI/graphics (F21's `ui/render.py` is not F22's; the CLI prints the render-ready text).

## 10. Failure behavior
- Unknown code ⇒ 'reason #N' (inline, D4).
- `reasonText` with a code outside the catalog ⇒ `nullptr` (callers must not print an empty string as if it were a reason).
- Completeness test cannot find `bw/state.h` ⇒ test FAILS with a named message (a skipped check is a failed check, D2).

Companion half (W5):
- Parity test cannot find `bw/state.h` or `bw/explain/reason_catalog.h` ⇒ FAIL with a named message (same D2 rule; never skip).
- Unknown reason bit ⇒ `reason #N` item at its own position (wording.py `UNKNOWN_REASON_PREFIX`), never hidden, never guessed.
- Unsupported event schema ⇒ `schema_supported == False`, no semantic rendering; `--seq` miss ⇒ exit code 1 + named message (CLI); `mask == 0` ⇒ "none" (a valid, non-fabricated rendering).
- Ambiguous `--seq` (same per-device seq on 2+ devices, no `--device` scope) ⇒ `AmbiguousSeq` with every candidate row, `render_ambiguous` text, CLI exit 1 — one of the rows is never picked (F22-AUD-1).

## 11. Budget
Flash < 2 KB. Verification: `pio run -e esp32dev` size before/after + `xtensa-esp32-elf-size -A` per object (Validate, numbers in the status log). Measured 2026-10-12: object footprint 843 B flash / 0 B RAM; image delta before consumers wire in +104 B. No new tasks, no RAM globals (table is `const`/`.rodata`), no heap (caller buffer only, D3).

Companion half (W5): no firmware impact (read-only Python, no new dependencies, no threads, no BLE calls — the CLI talks only to the local SQLite DB). Planned size ≈ 400–600 LOC including tests; runtime cost negligible (a bit-walk per rendered row, ≤ 32 iterations). Data stays on the machine (local-first, no logging of health values).

## 12. Out of scope
- Natural-language generation.
- Localization: English-only short strings `[ASSUMED]` — single-table design means a localized catalog later is a table swap, not a redesign.

## 13. Open questions / assumptions
- ~~Localization~~ closed 2026-10-12 (see §12).
- ~~Companion half (`companion/lib/features/explain/**`) deferred to W5~~ — **discharged**: planned 2026-10-17 (this entry, §6 D6–D9, §7/§8/§9 companion blocks); implementation follows in the W5 IMPLEMENT+TEST session over F21's scaffold (read-only DB).
- `rankContributions()` remains **deferred** with explicit reasons (D8): the synced events carry no contribution values; a ranked list needs a contract path (then CR-025). No `detector_id`/`FaultCode`/`schema_id` needed for the companion view; **CR-025 pre-allocated, unused**.
- No new `ReasonCode`, `EventType` or `detector_id` needed; **CR-017 stays reserved and unused** (all contracts used exist: `ReasonMask`, `ReasonCode` in `bw/state.h`, `Event.reasons`/`StateSnapshot.reasons` in `docs/interfaces/data_decision.md` §4.5).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Catalog completeness test in CI.

---
