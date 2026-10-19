# CR-022 — `DATASET_FORMAT.md` §7: export output (host, `tools/export`)

- **Raised by:** F28 (wave 5 export half, 2026-10-17), complete text below; spec context `docs/features/F28_data_export_research_mode.md` §16
- **Type:** additive (integrator authority, `05` §6) — append-only, `02` §6.6
- **Status:** **accepted 2026-10-17** (W5 G2, integrator; additive, `05` §6). Applied verbatim to `docs/contracts/DATASET_FORMAT.md` §7 + §6 version-history row on `master` `24e1f88`; CR index row added in `docs/change_requests/README.md` (`1ccc0c9`). The feature implementation never edits `DATASET_FORMAT.md` itself (H6 held: byte-identical until this CR was applied).

## Problem

`tools/export` (implemented this wave, spec §16) writes annotated CSVs whose
metadata rows (`#META,...`) and export manifest are not yet described in the
dataset contract. Without a §7, the export envelope has no contract home and
could drift from the tool that produces it.

## Proposal — new additive §7 in `docs/contracts/DATASET_FORMAT.md`

~~~markdown
## 7. Export output (host, `tools/export`)

An **export** turns a recorded session directory (§3) into schema-annotated
CSVs for ML development and debugging. It is observational only: data rows
are copied **verbatim** (timestamps, precision and `0`/`1` booleans are never
rewritten); alignment or resampling, if a consumer needs it, happens in
consumer code (`ml/`), never in the export layer.

### 7.1 `#META` rows

Every exported CSV starts with metadata rows, one per line:

```
#META,export,export_schema_version,<n>        # envelope version (this section)
#META,export,source_file,<name>.csv           # source file in the session dir
#META,session,<key>,<value>                   # repeat of the session.json scalar
                                              #   fields: session_id, schema_version,
                                              #   boot_id, firmware_version, started_utc,
                                              #   ended_utc, end_reason, raw_tee,
                                              #   raw_drops, imu_drops, ppg_drops,
                                              #   line_drops, feature_missed,
                                              #   malformed_rows (absent key -> empty
                                              #   cell, never 0)
#META,column,<idx>,<name>,<unit>,<type>       # 0-based column definitions; names equal
                                              #   the §3 per-file column lists
```

`<unit>` is a §2.3 unit or `-`; `<type>` is one of
`uint64|uint32|float|bool|enum|str|hex8|bitmask|verbatim`. Rows start with
`#`, so `pandas.read_csv(..., comment="#")` skips them, and the prefix stays
distinct from the device's `#HDR` so an exported file can never be misread as
a capture stream. Values are sanitised: `,`/newlines become `_`.

### 7.2 `export_manifest.json`

One JSON object per exported session: `export_schema_version`, `session_id`,
`session` (the verbatim `session.json` object), `exported_files` (list),
`absent_files` (expected §3 CSVs the session never had — e.g. `raw_ppg.csv`
with `raw_tee: false`; they are **recorded, never fabricated as empty
files**).

### 7.3 Exit-code map and lossy gate

`tools/export/cli.py` uses the same exit-code map as
`tools/replay/compare.py`: `0` ok · `2` lossy recording refused (session
`raw_drops > 0` or `malformed_rows > 0`; lifted by `--include-lossy`, and the
counters are then recorded in `#META`) · `4` missing `session.json`, unknown
`schema_version` (version printed), truncated/drifted source CSV, or an
existing non-empty `--out` target. `3` stays compare-only: export never
claims a comparison. Validation is exhaustive **before** any output file is
written (validate-then-write).

### 7.4 `EXPORT_SCHEMA_VERSION` semantics

`export_schema_version` versions the **export envelope** (`#META` rows +
manifest), independent of the dataset `schema_version` (§4). A bump is
additive metadata only — new `#META` kinds or manifest fields — never a
data-row or data-column change. Readers must ignore unknown `#META` kinds.
~~~

Plus one row in §6 version history:

~~~markdown
| 1 | ADD export half 2026-10-17 (additive; line grammar and `schema_version` unchanged): §7 export output — `#META` row grammar, `export_manifest.json`, exit-code map, `EXPORT_SCHEMA_VERSION` |
~~~

## Migration / compatibility

Purely additive: no §2 grammar, §3 layout, §5 vector or `schema_version`
change. `schema_version` stays **1**. Existing capture/replay tools and the
accepted FIX addendum are untouched; `tools/export` already produces exactly
what §7 describes (pinned by `tools/export/tests/test_metadata_rows.py`
against §3, and by the suite-3 vector chain).

## Affected features

F28 (author; `tools/export` produces this format) · F05/F06/F09/F29/F14 and
`ml/` (consumers of exported datasets) · F21 (future SQLite source of the
same export shape, follow-up).

## Test plan

Already green in this wave: `tools/export/tests/test_metadata_rows.py`
(#META grammar + §3 column drift guard both directions),
`test_export_csv.py` (verbatim rows, lossy gate, refusal paths),
`test_events_bin.py` (vector ↔ `decode_events` ↔ adapter chain),
`test_export_cli.py` (exit codes 0/2/4, `--include-lossy`, target
protection). After the CR is applied, suite 1 gains a §7 parse (the plan's
build step 4) — until then it pins §3 only, per §16.
