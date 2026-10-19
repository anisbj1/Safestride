# Feature schema contract

> **Status: DRAFT (Design stage, 2026-10-03).** Owner: **F05**. Initial draft: `docs/02_INTERFACES.md` §6.5. Expanded per F05 spec §14 (DoD). While DRAFT, edits are free (F05 owns this file); once the integrator marks it **Accepted**, changes go through a change request.

## 1. Purpose and scope

Single source of truth for how a `FeatureVector` is defined, normalized and validated:

- `ml/common/schemas/feature_schema_v<N>.json` — the schema itself (drives Python training **and** the generated C++ constants, F05 D2). **Normative path:** shared/test-fixture schemas live in `ml/common/schemas/`; a feature may keep its production schema under `ml/<feature>/schemas/` (F06: `ml/wakefulness/schemas/`) until F05's generator learns multi-root export at M5 — one file move either way.
- The generated header layout the composition root injects into the feature assembler (F05 D4).
- The normative normalization formula and the runtime handshake rules (`Err` table, §3).
- Test vectors usable by firmware (`pio test -e native`) and companion/Python tests alike.

Out of scope: how features are *computed* from sensors (F06 assembly), model weights (per-model generated headers), and any clinical meaning — this is a data-format contract for a non-medical academic prototype.

Consumers: feature assembler (F06, later F09), `ml/` training code, `export_cpp.py`, `wire_ml.*` (app wiring).

## 2. Layout, versioning, registry

### 2.1 JSON schema (`feature_schema_v<N>.json`, UTF-8, decimal numbers)

```json
{
  "schema_id": 2,
  "schema_version": 1,
  "features": [
    { "name": "motion_energy", "unit": "g_rms", "source": "imu",
      "normalization": { "method": "standardize", "mean": 100.0, "std": 10.0 } },
    { "name": "hr_bpm", "unit": "bpm", "source": "ppg",
      "normalization": { "method": "none" } }
  ]
}
```

(The committed worked example — `ml/common/schemas/feature_schema_v2.json`, the F05 test fixture, registry row §2.4 id 2.)

| Field | Type / rule |
|---|---|
| `schema_id` | `uint16`, **≠ 0** (0 = invalid, never issued), allocated in the registry §2.4 |
| `schema_version` | `uint16`, **≠ 0**; incremented on **any** change below (§2.3) |
| `features` | ordered array, 1 … `kMaxFeatures` (32, `bw/model.h`); **array index = `FeatureVector` position — order is normative** |
| `features[].name` | non-empty, snake_case, **unique within the schema** |
| `features[].unit`, `features[].source` | non-empty strings (documentation/assembly metadata; not checked at runtime) |
| `features[].normalization.method` | `"none"` \| `"standardize"` (anything else = generation-time error, F05 D4) |
| `…standardize.mean/std` | finite floats, **`std > 0`** (else generation-time error); `method: "none"` must **not** carry `mean`/`std` (they would be silently ignored) |

**File naming: the `N` in `feature_schema_v<N>.json` is the `schema_id`,** not the version —
a schema keeps one file while `schema_version` grows, and a future problem domain
(id 3, …) gets its own file instead of colliding. Generated-header names follow the file
(§2.2). Schema files are cross-checked against every model that references them at export
time (`export_cpp.py check`: unknown id, version mismatch and feature-count mismatch are
hard errors).

Endianness: not applicable — JSON is UTF-8 text and generated headers are C++ source compiled per target. `FeatureVector` floats are in-memory, target-native, and are **not** persisted or transmitted (persisted records are `Event`, `docs/interfaces/data_decision.md` §4.5).

### 2.2 Generated C++ header (checked in, byte-stable, F05 D8)

Path: `firmware/lib/bw_ml/include/bw/ml/`.

`feature_schema_v<N>.h` (N = schema id):

```cpp
namespace bw::ml::schema_v2 {          // one nested namespace per schema id:
constexpr uint16_t kSchemaId = …;      //   two schema headers may share a TU,
constexpr uint16_t kSchemaVersion = …; //   flat bw::ml would redefinition-error
constexpr uint8_t  kFeatureCount = …;  //   (pinned in the Implement stage)
constexpr const char* kFeatureNames[kFeatureCount] = { … };   // JSON names, index = position
constexpr float kFeatureMean[kFeatureCount] = { … };          // 0.0f for method "none"
constexpr float kFeatureStd[kFeatureCount]  = { … };          // 1.0f for method "none"
}
```

`model_<name>_v<N>.h` (N = `ModelDescriptor.model_version`), one nested namespace per
model (`bw::ml::<name>`), containing: the weight/tree constant arrays (`kWeights`/
`kBias`, or `kNodes`/`kLeaves`/`kTreeViews`), `kClassNames[]`, an initialized
`ModelDescriptor kDescriptor`, the ready-to-use payload
`LinearModelData kModel` / `TreeModelData kModel` (so wiring and tests construct the
model with one line), and:

```cpp
constexpr size_t kModelBytes = …;      // exporter-computed numeric-array bytes only —
                                        // pointers/strings are target-dependent and excluded
static_assert(kModelBytes <= kMaxModelBytes, …);                 // §8 budget (D6/D8)
static_assert(kModelBytes == sizeof(kWeights) + sizeof(kBias), …); // layout cross-check:
                                        // any drift between exporter and C++ fails every build
```

`method: none` is **rewritten to identity at generation** (mean 0, std 1), so firmware always applies the single formula in §4 — there is no runtime branch on `method`.

### 2.3 Versioning rules

- Same `schema_id`, bump `schema_version` whenever feature **order, count, definition, unit, source, or normalization method/parameters** change. Never reorder without a version bump (ml-pipeline contract).
- New problem domain (e.g. a cataplexy feature set) → new `schema_id`; ids are never reused or repurposed.
- Model weights are versioned separately (`ModelDescriptor.model_version`, training-data hash) — retraining with an unchanged schema does not bump `schema_version`.
- Runtime: `predict()`/`contributions()` compare **both** `schema_id` and `schema_version` plus `count` against the model's embedded metadata; mismatch → `Err::InvalidArg`, output untouched (F05 D3).

### 2.4 Schema id registry (F05 allocates)

| id | feature set | defining feature | status |
|---|---|---|---|
| 0 | — | — | reserved, always invalid |
| 1 | alertness feature set | F06 | **confirmed by the integrator 2026-10-04** — file `ml/wakefulness/schemas/feature_schema_v1.json` (`schema_version` 1, 16 features, `bw_wake/alertness_schema.h`); the path stays feature-local per §1 until F05's generator learns multi-root export |
| 2 | F05 test fixture — the 2-feature worked example (§2.1/§4), **non-production** | F05 | allocated 2026-10-04 (re-identification, §5 v1.2; it previously claimed id 1 and collided with row 1) — file `ml/common/schemas/feature_schema_v2.json`, generated `feature_schema_v2.h` / `bw::ml::schema_v2`; never wired into the app |

## 3. Error and edge-case behavior

| Condition | When | Result |
|---|---|---|
| `schema_id` / `schema_version` mismatch | `predict`, `contributions` | `Err::InvalidArg`; output argument untouched |
| `count != kFeatureCount` (incl. 0, +1, 255) | `predict`, `contributions` | `Err::InvalidArg` — checked **before** `v[]` is read (bounds) |
| NaN / Inf in `v[]` | `predict`, `contributions` | `Err::InvalidArg`; never propagated |
| `out == nullptr` or `max == 0` | `contributions` | `Err::InvalidArg` |
| Degenerate embedded metadata (0 classes, 0 trees, 0/`>` bounds on feature count or class count, null `class_names`, id/version 0) | `predict` | `Err::Corrupt` (defensive; generated models never contain it) |
| Corrupt tree structure: node/leaf/feature index outside its array, view outside the model arrays, or a cycle (traversal beyond `kMaxTreeDepth + 1` visits) | `predict` | `Err::Corrupt` (defensive; bounds every generated array before it is read) |
| Non-finite intermediate score (linear logits, or a tree's soft-voted sum) from embedded constants | `predict` | `Err::Corrupt` — reachable only with corrupt weights/leaves or an input wildly outside the training domain; max-shift already handles every finite logit (D7) |
| `contributions()` on a non-linear model | `contributions` | `Err::Unsupported` (allowed by `bw/model.h`) |
| Unknown normalization method, `std ≤ 0`, duplicate names, `count > 32`, id/version 0, model over `kMaxModelBytes` | `export_cpp.py` (generation time) | hard error, **no header written** — a bad schema can never reach firmware |
| Tree deeper than `kMaxTreeDepth`, more than `kMaxTrees`, leaf rows ≠ `n_classes`, leaf sum ≠ 1 ± 1e-6, leaf outside 0..1, `feature` outside the schema, non-finite constant, file name ≠ embedded name/version, model ↔ schema id/version/count mismatch | model load / `export_cpp.py` | `ModelError`/`ValueError` naming file and field — no header written |
| Checked-in header older than the schema/weights | `pio test -e native` / pytest | regeneration byte-compare fails (staleness gate, F05 D8) |
| Missing/unparsable JSON in tooling | `ml/common/*.py` | exception naming file path + field |

**Leaf value range is export-time only:** `0..1` per entry and row sum `1 ± 1e-6` are validated by `export_cpp.py`/`load_model` at generation (row above) — at runtime `predict()` re-checks only finiteness and refuses a non-finite score with `Err::Corrupt` (no per-leaf table scan on device, D8 budget).

Missing input at assembly time is **not** F05's decision: the assembler must not invent values — it either refuses (`Err`) or emits a vector it can vouch for; degraded-input confidence reduction is F06's (F05 §10).

## 4. Test vectors (worked example)

Oracle: `ml/common/reference.py`. Tolerances are named constants: parity `kParityTolerance = 1e-5`, sum `kProbSumTolerance = 1e-5`. Values below are hand-checkable; the committed fixture set (Implement stage) extends them.

**Input.** Schema of §2.1 (registry §2.4: the F05 fixture, `schema_id` 2), `FeatureVector = {schema_id: 2, schema_version: 1, count: 2, v: [1.0, 72.0], t_ms: 12345}` where raw values were `motion_energy = 110.0`, `hr_bpm = 72.0`.

**Normalization (firmware/assembler side, §2.2 constants):**

```
x0 = (110.0 − 100.0) / 10.0 = 1.0     x1 = (72.0 − 0.0) / 1.0 = 72.0   → v = [1.0, 72.0]
```

**A. Linear/softmax toy model.** `class0: w = [0, 0], b = 0` · `class1: w = [1, 0], b = 0`.

```
z = [0.0, 1.0]   max z = 1.0   exp(z − max) = [0.36787944117144233, 1.0]   Σ = 1.3678794411714423
p = [0.26894142, 0.73105858]        predicted class = 1        confidence = 0.73105858
contributions (predicted class, feature order) = [(0, 1.0·1.0 = 1.0), (1, 0.0·72.0 = 0.0)]
```

(`Σp = 1.00000000`; class-0 probability is the familiar sigmoid(−1) = 0.26894142.)

**B. Tree-ensemble toy model.** Two depth-1 trees, split on feature 0 at threshold 0.5; leaf vectors `p[class0, class1]`:

| tree | left leaf (x0 ≤ 0.5) | right leaf (x0 > 0.5) |
|---|---|---|
| 1 | [0.9, 0.1] | [0.2, 0.8] |
| 2 | [0.5, 0.5] | [0.4, 0.6] |

`x0 = 1.0 > 0.5` → both trees take their right leaf → soft vote (per-tree average):

```
p = [(0.2 + 0.4) / 2, (0.8 + 0.6) / 2] = [0.3, 0.7]   predicted class = 1   confidence = 0.7
contributions() = Err::Unsupported
```

**Expected outputs** (both models): `n_classes = 2`; probabilities within `kParityTolerance` of the values above; `Σp = 1 ± kProbSumTolerance`. A second run must be bit-identical (determinism test).

## 5. Version history

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-10-03 | Design-stage expansion from stub: JSON layout + validation rules, generated header layout, versioning + id registry, error/edge table, worked test vectors (F05 session 1). Status DRAFT. |
| 1.1 | 2026-10-03 | Implement stage: file naming pinned (N = `schema_id`; `method: "none"` must not carry parameters); generated-header namespaces pinned (`bw::ml::schema_vN` / `bw::ml::<model>`) with `kModel` payload + `kModelBytes` semantics (numeric arrays only, compile-time `sizeof` cross-check); error table extended with tree-structure and non-finite-score `Err::Corrupt` rows plus the model-load validation list; registry note for id 1 fixtures. |
| 1.2 | 2026-10-04 | **Re-identification of the F05 test fixture (integrator decision):** row 1 is confirmed for F06's alertness set (`ml/wakefulness/schemas/feature_schema_v1.json`, 16 features), so F05's 2-feature fixture moved from id 1 to **id 2** — `ml/common/schemas/feature_schema_v1.json` → `feature_schema_v2.json` (`schema_version` unchanged at 1), generated `feature_schema_v1.h` → `feature_schema_v2.h` / `bw::ml::schema_v2`, fixture models and `models_under_test.h` updated to `kTestSchemaId = 2`; §1 gained the normative-path sentence (shared fixture schemas in `ml/common/schemas/`, feature-local production schema until multi-root export at M5) and §3 the export-time-only leaf-range note. |
