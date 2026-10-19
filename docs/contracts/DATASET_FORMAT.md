# Dataset and capture format

> **Status: Draft** (was STUB). Owner: **F28**. Schema `v1`, produced by
> `bw_research` (line format) and consumed by `tools/capture` and
> `tools/replay`. This file is the single source of truth for the grammar;
> the same literals are asserted by `firmware/test/bw_research` (golden
> tests) and by `tools/capture/tests/test_lines.py`. Once marked **Accepted**
> it is contract-locked: changes go through a change request.

## 1. Purpose and scope

A serial research capture is a byte stream on the device UART containing one
kind of content: research rows. Everything else that other code prints to the
same port (bench diagnostics such as `[PIPE]`/`[EVT]`/`[BTN]` lines, boot
banner) is **not** research data and is diverted by `tools/capture` into a
separate diagnostics log — never into the dataset (F28 D6).

The dataset is **observational only**: rows carry recorded values with their
quality flags. Nothing here is a diagnosis; events are labelled
"possible"/"suspected" with a confidence value (00 §1).

## 2. Line grammar (schema v1)

Every row is one line: ASCII fields separated by `,`, terminated by `\n`,
NUL-terminated in memory, at most `kMaxLineLen` (256) bytes including `\n`.
All integer fields are unsigned decimal; `TimeMs` values print as `uint64`
(0 … 18446744073709551615). Enum fields print **names**, not numbers
(§2.3). A formatter never emits a partial line: if a row does not fit the
buffer it is dropped whole and counted (`#HDR` `line_drops`).

### 2.1 Row types

```
#HDR,<schema>,<boot_id>,<fw_version>,<imu_drops>,<ppg_drops>,<line_drops>,<feat_missed>
I,<t_ms>,<ax_g>,<ay_g>,<az_g>,<gx_dps>,<gy_dps>,<gz_dps>
P,<t_ms>,<red>,<ir>
F,M,<t_ms>,<quality>,<enmo_g>,<mag_std_g>,<jerk_rms_g_per_s>,<gyro_rms_dps>,<gravity_tilt_deg>,<dominant_freq_hz>,<stillness_ms>
F,P,<t_ms>,<quality>,<hr_valid>,<hr_bpm>,<hr_confidence>,<perfusion_index>,<ir_dc_level>,<sqi>,<motion_corrupted>,<hrv_valid>,<rmssd_ms>
F,C,<t_ms>,<activity>,<confidence>
E,<seq>,<t_start_ms>,<boot_id>,<duration_ms>,<t_start_unix>,<confidence>,<reasons>,<payload_hex>,<quality_imu>,<quality_ppg>,<type>,<label>,<activity>,<alertness>,<detector_id>,<schema_version>
M,<t_ms>,<marker>
```

- `#HDR` is emitted on session activation and every `kHeaderRepeatMs`
  (30 s). It carries the cumulative drop counters, so a capture tool that
  attaches late (or a session cut with Ctrl+C) still learns the loss that
  happened — `raw_drops` in `session.json` is `imu_drops + ppg_drops +
  line_drops` **as of the last `#HDR` received** (age in seconds is
  recorded). `fw_version` is sanitised: `,`, `\n`, `\r` become `_`, longer
  than 32 chars is truncated (field-count stability).
- `I`/`P` come only from the F28-owned raw tee (D3), never from
  `ctx.imu_samples`/`ctx.ppg_samples` (single-consumer hazard, D5). The
  `PpgSample.saturated` flag is **not** transmitted in v1 — known
  limitation; saturation shows up as clipped raw values in the data.
- `F,M` / `F,P` / `F,C` are the 1 Hz feature rows (motion / PPG / context).
  `F,C` carries only the activity estimate in v1; wear/alertness/mode
  columns arrive with a schema bump once an `IStateProvider` exists.
- `E` is a decoded `bw::Event` (48-byte layout contract). `payload_hex` is
  the 8-byte payload as 16 lower-case hex chars in memory order
  (little-endian, see §2.2).
- `M` marks a moment: the device emits `M,<t_ms>,user_marked#<seq>` when a
  `UserMarked` event is captured; the host side can add its own markers
  during a capture (`--marker`), which land in `markers.csv` with
  `source=host`.

### 2.2 Byte order and sizes

Little-endian on both ends (xtensa ESP32 and x86/ARM hosts). There is no
binary framing on the wire in M1: rows are plain text. The 48-byte `Event`
binary layout (offsets in `contract_checks.h`) matters for `decode_events.py`
and future BLE export; `payload_hex` reproduces those 8 bytes verbatim.

### 2.3 Number formats and enum spellings

| Field class | Format | Example |
|---|---|---|
| `t_ms`, `seq`, `boot_id`, `duration_ms`, `stillness_ms`, counters | unsigned decimal | `123456789` |
| g values (`ax..az`, `enmo_g`, `mag_std_g`, `jerk_rms_g_per_s`) | `%.4f` | `-0.0034` |
| dps / degrees / bpm (`gx..gz`, `gyro_rms_dps`, `gravity_tilt_deg`, `hr_bpm`, `rmssd_ms`, `ir_dc_level`) | `%.1f` | `12.3` |
| confidences / SQI / perfusion-related ratios | `%.3f` | `0.750` |
| `perfusion_index` | `%.5f` | `0.04567` |
| `dominant_freq_hz` | `%.3f` | `1.250` |
| booleans (`hr_valid`, `motion_corrupted`, `hrv_valid`) | `0` or `1` | `1` |
| `reasons` | decimal `ReasonMask` (bits of `bw::ReasonCode`) | `4100` |

Enum spellings (append-only, 02 §6.6 — a new enum value requires a schema
note here):

- `quality` ∈ `Good` | `Degraded` | `Insufficient` (`bw::DataQuality`)
- `activity` ∈ `Unknown` | `Stationary` | `Sitting` | `Standing` |
  `Walking` | `Running` | `LyingResting` | `InactiveProlonged`
- `type` ∈ `SleepEpisode` | `DrowsinessEvent` | `SuspectedCataplexy` |
  `UserMarked` | `AlertRaised` | `SensorFault` | `DeviceNotWorn` |
  `BatteryWarning` | `NapPlanned` | `NapCompleted` | `TimeSync` | `Boot`
- `label` ∈ `Unlabeled` | `Confirmed` | `Rejected`
- `alertness` ∈ `Unknown` | `NotWorn` | `Awake` | `Drowsy` |
  `HighDrowsinessRisk` | `PossibleSleep`

### 2.4 Classification rule (what counts as a research row)

`tools/capture` accepts a line into the dataset iff it matches `^#HDR,` or
`^[IPFEM],` **and**, for `F` rows, the second field is one of `M`,`P`,`C`
and, for `I`/`P`/`E`/`M` rows, the second field starts with a digit or `-`.
Everything else — including malformed `F` rows and all bench diagnostics —
goes to `diagnostics.log`. A line that matches the prefix but fails to
parse is counted as `malformed_rows` and **discarded, never repaired**
(F28 §10); the row is also written to `diagnostics.log` for inspection.

## 3. Session directory

```
<out>/research_<session_id>/        # session_id = UTC start, e.g. research_20261011T101500Z
  session.json                      # metadata, counters, end reason
  raw_imu.csv                       # t_ms,boot_id,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps
  raw_ppg.csv                       # t_ms,boot_id,red,ir
  features.csv                      # union columns, one row kind per line (see below)
  events.csv                        # one row per Event, same fields as the E line
  markers.csv                       # t_ms,boot_id,marker,source (source ∈ device|host)
  headers.log                       # every #HDR line verbatim, as received
  diagnostics.log                   # everything that was NOT research data
```

`features.csv` columns (empty string = not applicable to that kind, never
`0` — an absent value must stay distinguishable from a real zero):

```
t_ms,boot_id,kind,quality,enmo_g,mag_std_g,jerk_rms_g_per_s,gyro_rms_dps,
gravity_tilt_deg,dominant_freq_hz,stillness_ms,hr_valid,hr_bpm,hr_confidence,
perfusion_index,ir_dc_level,sqi,motion_corrupted,hrv_valid,rmssd_ms,
activity,confidence
```

with `kind` ∈ `motion` | `ppg` | `context` (the `F,M`/`F,P`/`F,C` subtype).

`session.json` (schema `v1`):

```json
{
  "schema_version": 1,
  "session_id": "research_20261011T101500Z",
  "boot_id": 0,
  "firmware_version": "0.1.0",
  "started_utc": "2026-10-11T10:15:00Z",
  "ended_utc": "2026-10-11T10:25:00Z",
  "end_reason": "user",
  "row_counts": {"header": 21, "imu": 30000, "ppg": 60000, "motion": 600,
                 "ppg_features": 600, "context": 600, "event": 3, "marker": 1},
  "raw_drops": 0,
  "imu_drops": 0,
  "ppg_drops": 0,
  "line_drops": 0,
  "feature_missed": 0,
  "drop_history": [{"at_s": 12, "imu_drops": 0, "ppg_drops": 0,
                    "line_drops": 0, "feature_missed": 0}],
  "imu_period_ms": 20,
  "imu_missing_samples": 0,
  "ppg_period_ms": 10,
  "ppg_missing_samples": 0,
  "malformed_rows": 0,
  "diagnostics_lines": 142,
  "raw_tee": true,
  "counters_as_of_hdr_seconds": 12
}
```

- `boot_id` `0` = **unknown** (no F17 source is wired yet, spec §13); it
  becomes the F17 counter once available. `#HDR` boot_id changes start a new
  session directory, as does a `schema_version` change.
- `raw_tee` records whether `I`/`P` rows were expected at all (the §5
  gate): a session with `false` is a features/events-only capture, not a
  truncated one.
- `imu_drops` / `ppg_drops` / `line_drops` / `feature_missed` are the
  per-source split **as of the last `#HDR`**; `raw_drops` stays
  `imu_drops + ppg_drops + line_drops` (§2.1, unchanged). `drop_history`
  appends one sample per `#HDR` received (`at_s` = host seconds since
  capture start), so the loss curve survives the capture — together with
  the verbatim `headers.log` a session dir is self-describing (F28 FIX
  2026-10-13).
- `imu_period_ms` / `ppg_period_ms` and `*_missing_samples` are the
  **host-side loss cross-check**: the modal positive `t_ms` delta of the
  captured rows is the device sample period, each gap spanning k periods
  hides k-1 sample slots. Derived only from the CSV, independent of the
  device counters (a disagreement between the two is a finding, never
  repaired); `null` when fewer than 2 rows exist.
- `end_reason` ∈ `user` (Ctrl+C) | `schema_change` | `boot_change` |
  `eof` (input stream ended).

## 4. Error and edge-case behavior

| Situation | Behavior |
|---|---|
| Capture attached mid-session | First `#HDR` arrives within 30 s; rows before it are not retro-creatable |
| Ctrl+C | `session.json` finalized with `end_reason: user`; counters as of last `#HDR` |
| Device resets mid-capture | New `boot_id` (once F17 exists) ⇒ new session directory; today: new session on `#HDR` gap + timestamp jump, old file gets `eof` |
| Dropped rows (`raw_drops > 0`) | Recorded; **comparison/replay is invalid**, never reported as a pass (`tools/replay/compare.py` exits 2); the per-source split and its curve are kept (`imu_drops`/`ppg_drops`/`drop_history`) and cross-checked against the CSV gaps (`imu_missing_samples`) |
| Malformed rows | Counted + diverted to `diagnostics.log`; never repaired or guessed |
| Unknown enum name | Parser counts the row malformed (enums are append-only; a new name means schema drift) |
| Unknown `schema_version` | Capture tool refuses to start the dataset (`compare.py` exit 4) with the version printed |
| Same `t_ms` seen twice (mailbox race) | Second row suppressed on device; if it still occurs the CSV keeps both rows (timestamp + kind are not unique keys) |

## 5. Test vectors (golden lines, schema v1)

Asserted by `firmware/test/bw_research/test_bw_research/test_line_format.cpp`
and mirrored by `tools/capture/tests/test_lines.py`:

```
#HDR,1,7,0.1.0,2,3,4,5
#HDR,1,0,unknown,0,0,0,0
I,1000,0.0012,-0.0034,0.9988,1.5,-2.3,0.0
P,2000,123456,654321
F,M,5000,Good,0.0123,0.0456,0.0789,12.3,45.6,1.250,700
F,P,6000,Degraded,1,63.5,0.875,0.04567,12345.6,0.900,0,0,0.0
F,C,7000,Walking,0.750
E,3,123456789,7,5000,1700000000,0.500,4100,deadbeef00010203,Good,Insufficient,UserMarked,Confirmed,Walking,Awake,5,1
M,8000,user_marked#42
```

`decode_events.py` test vector — the 48-byte `Event` binary layout
(offsets pinned by `contract_checks.h`, little-endian), built from a
zero-initialised struct with exactly these fields set. The canonical bytes
below are parsed by `tools/capture/tests/test_decode_events.py`
(`test_vector_in_doc_matches_decoder`): editing either these bytes or
`decode_events.py` alone fails that test. The marker line is pinned there —
do not reformat this block without updating the test in the same commit.

```text
F28-EVENT-VECTOR-48B
15cd5b0700000000 03000000 07000000 88130000 00f15365 0000003f 04100000 deadbeef00010203 0002030104020501
```

Field breakdown of the same bytes (offsets are of the little-endian `Event`
struct):

```
t_start_ms   = 123456789      -> bytes 0..7
seq          = 3              -> bytes 8..11
boot_id      = 7              -> bytes 12..15
duration_ms  = 5000           -> bytes 16..19
t_start_unix = 1700000000     -> bytes 20..23
confidence   = 0.5f           -> bytes 24..27
reasons      = 4100           -> bytes 28..31
payload      = DE AD BE EF 00 01 02 03 -> bytes 32..39
quality.imu  = Good(0)        -> byte 40
quality.ppg  = Insufficient(2)-> byte 41
type         = UserMarked(3)  -> byte 42
label        = Confirmed(1)   -> byte 43
context      = Walking(4)     -> byte 44
alertness    = Awake(2)       -> byte 45
detector_id  = 5              -> byte 46
schema_version = 1            -> byte 47
```

Expected Python-side fields: `confidence == 0.5`, `reasons == 4100`,
`payload_hex == "deadbeef00010203"`, `type == "UserMarked"`,
`label == "Confirmed"`, `schema_version == 1`.

## 6. Version history

| Version | Change |
|---|---|
| 1 | Initial grammar: `#HDR` + `I`/`P`/`F,M`/`F,P`/`F,C`/`E`/`M` rows, session directory + `session.json`, drop counters in `#HDR` (this file) |
| 1 | FIX addendum 2026-10-13 (additive; line grammar and `schema_version` unchanged): `headers.log`, `session.json` per-source counters + `drop_history` + host-side gap cross-check (`*_period_ms`/`*_missing_samples`) |
| 1 | ADD export half 2026-10-17 (CR-022, additive; line grammar and `schema_version` unchanged): §7 export output — `#META` row grammar, `export_manifest.json`, exit-code map, `EXPORT_SCHEMA_VERSION` |

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
