# F05 — ML inference engine (+ Python ML tooling foundation)

> Track **B** · Milestone **M2** · Runs on **firmware + ml** · Contract changes only via change request.

## 1. Responsibility
A model-agnostic runtime that takes a `FeatureVector` and returns interpretable probabilities, plus the Python side that trains/exports models against a versioned feature schema. Knows nothing about sensors, states or alerts.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `FeatureVector` (schema id/version)
- Generated model headers from `ml/`

**Outputs (produced contracts)**
- `IModel` implementations: `LinearSoftmaxModel`, `TreeEnsembleModel`
- `ModelOutput` (probabilities + confidence)
- `contributions()` for linear models (feeds F22)
- `feature_schema_v<N>.json` + C++ constant generator + model exporter (Python)

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Know sensor names or state semantics.
- Control hardware or decide alerts.
- Load a model whose schema id/version mismatches the caller.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Model family**
- Simplest: Hand-written weights for a linear model.
- Professional/scalable: Registry of versioned models loadable from flash, with TFLite Micro backend.
- Trade-offs: TFLM costs RAM/flash and needs data we lack; hand weights are unreproducible.
- **Choice:** Exporter-generated constant arrays for linear/softmax and shallow trees (DEC-006); `IModel` hides the backend so TFLM can be added later.

**D2. Model/schema coupling**
- Simplest: Comment 'feature order: …'.
- Professional/scalable: Schema JSON is the single source for training code and firmware constants; model metadata embeds schema id/version and training-data hash.
- Trade-offs: Extra tooling for a class of bug that otherwise appears silently.
- **Choice:** Schema JSON + generator; runtime check on load.

**D3. Where the schema handshake is enforced**
- Problem: `IModel` exposes only `schemaId()`; §6.5 requires refusing a model whose `schema_id/version` does not match the assembler's, and a wrong `count` would read past `FeatureVector::v[]`.
- Simplest: check `schema_id` only, inside `predict()`.
- Scalable: extend `IModel` with `schemaVersion()`, `featureCount()` and a `descriptor()` so callers can pre-flight a model — via change request (contract is locked).
- Trade-offs: id-only misses exactly the silent breakage D2 targets (features added/reordered under one problem id); a CR widens a locked contract for a consumer (F06) that only ever calls `predict()`; per-implementation checks keep enforcement in one place with zero contract change.
- **Choice:** `predict()` and `contributions()` validate all three — `schema_id`, `schema_version`, `count == expected feature count` — **before** touching `v[]`; mismatch → `Err::InvalidArg` (§9). Degenerate embedded metadata (0 classes, 0 trees) → `Err::Corrupt`. Simplest = id-only (rejected: silent feature-set drift). Scalable = widened `IModel` (deferred until a consumer needs pre-flight metadata; then a CR).

**D4. Who normalizes — the semantics of `FeatureVector::v[]`**
- Problem: the schema carries per-feature `normalization`; whether `v[]` holds raw or training-space values is undefined; train/serve drift then appears silently.
- Simplest: fold `mean/std` into the exported weights/thresholds at export time — consumers pass raw values, the model is self-contained.
- Scalable: a model-internal preprocessing pipeline (clip, impute, dynamic transforms) — raw in, probabilities out, any future transform model-internal.
- Trade-offs: folding only covers *static* transforms — it cannot express user-baseline-relative features (F13 data exists only at assembly time), it bakes input-domain knowledge into every backend (a future TFLM model would still expect normalized inputs), and diagnostics cannot inspect the input domain; a model-internal pipeline couples `IModel` to runtime providers (violates "the model knows nothing") for exactly two supported methods; assembler-side keeps the standard artifact (weights defined on a fixed input domain) and reuses the injection path that §6.5 requires anyway (the assembler already carries schema id/version).
- **Choice:** assembler-side. `FeatureVector::v[]` always carries **training-space (normalized) values**; the composition root injects the generated constants (`mean`, `std`, id, version) into the feature assembler; the model is pure math on that domain. The generator rewrites `method: none` to identity (`mean 0`, `std 1`) so firmware applies one normative formula `x' = (x − mean) / std`, documented with test vectors in `docs/contracts/FEATURE_SCHEMA.md` §4. Cross-feature: F06 must apply it ([ASSUMED], integrator verifies). Simplest = fold-in (rejected: static-only, hides the input contract). Scalable = in-model pipeline (rejected: layer coupling, over-engineering for {none, standardize}).

**D5. Definition of `ModelOutput.confidence`**
- Problem: the contract says only `float confidence; // 0..1`.
- Simplest: `confidence = max_c p[c]`.
- Scalable: calibrated confidence (temperature scaling / reliability curve learned once F29 has labeled data).
- Trade-offs: max-p ignores distribution shape, but with 2–3 classes it is nearly the top-1 margin; entropy/margin add machinery with no data to justify it; calibration is impossible today (no labeled set exists).
- **Choice:** `confidence = max p`, tagged `[EXPERIMENTAL]`, documented as *model certainty, never clinical certainty*. Reducing confidence for degraded inputs is F06's job (`AlertnessProbabilities.input_quality`), not the model's. Simplest = max-p (chosen); scalable = calibrated confidence (defer to F29).

**D6. Tree ensemble representation and aggregation**
- Problem: express up to `kMaxTrees` × depth `kMaxTreeDepth` trees as static flash constants, turn votes into probabilities, and bound size.
- Simplest: flat `TreeNode`/`TreeLeaf` arrays per tree (root at index 0) with **soft voting** — average the leaf probability vectors of all trees.
- Scalable: a serialized model blob in a private format interpreted by a generic parser/VM — arbitrary structures and later backends behind one reader.
- Trade-offs: soft averaging yields a proper probability distribution directly (`ModelOutput` needs probabilities; majority vote needs an artificial conversion) and matches the Python oracle 1:1; a parser adds size, failure modes and a second format to test for zero present need.
- **Choice:** flat arrays + soft averaging. `kMaxTreeDepth`/`kMaxTrees` are hard limits checked at export; the exporter also enforces `kMaxModelBytes` (§8) because a single full depth-8 tree is ~511 nodes — bytes must be bounded where they are produced. Traversal runs ≤ `kMaxTreeDepth` steps (termination test). `contributions()` → `Err::Unsupported` (contract-sanctioned for non-linear models). Simplest = chosen; scalable = blob/VM (rejected: YAGNI at ≤32 trees).

**D7. Linear model numerics and `contributions()` semantics**
- Problem: define logits/softmax robustly and give `Contribution{feature_index, weighted_value}` a precise meaning for a multiclass model.
- Simplest: `z_c = b_c + Σ w_c,i·x_i` with plain softmax; contributions = `w·x` of the predicted class.
- Scalable: expose per-class contribution arrays via an API extension (`classIndex` argument) through a CR.
- Trade-offs: plain softmax overflows to Inf/NaN on large logits — NaN propagation is forbidden; per-class output needs a contract change for a consumer (F22) that ranks exactly one decision.
- **Choice:** max-shift softmax (`z_c − max z` before `exp`; algorithmic, no named constant needed), probabilities sum to 1 within the test tolerance. `contributions()` returns, in feature order, `w_pred,i · x_i` for the **predicted** class (argmax p), capped at `max`; the bias is excluded (it is not a feature); `out == nullptr || max == 0` → `Err::InvalidArg`; schema mismatch → `Err::InvalidArg`; on any `Err` the output arguments are left untouched. Simplest = chosen; scalable = per-class API via CR (deferred, no consumer yet).

**D8. Generated artifacts: location, float fidelity, staleness**
- Problem: schema JSON, Python training and firmware constants must not drift; exported floats must survive the round-trip for the 1e-5 parity test.
- Simplest: check generated headers into the repo, regenerate by hand when the schema/model changes.
- Scalable: build-time codegen (PlatformIO `extra_scripts`) so a stale header is impossible, plus CI provenance (git + training-data hash in the descriptor).
- Trade-offs: checked-in files can go stale silently; build-time generation hides artifacts from review and couples both build envs to a Python toolchain.
- **Choice:** checked-in generated headers under `firmware/lib/bw_ml/include/bw/ml/` (`feature_schema_v<N>.h`, `model_<name>_v<N>.h`), produced deterministically (byte-stable) by `export_cpp.py`; **staleness is caught by tests** — the exporter round-trip test regenerates and byte-compares against the checked-in file, so a stale header fails `pio test -e native`. Floats are emitted as `repr(float(np.float32(v)))` (shortest literal that round-trips double→float bit-exactly); the test asserts bit-exact constants. Descriptor embeds `training_data_hash` + `schema_id/version` (D2). Simplest = checked-in + test gate (chosen); scalable = build-time codegen + CI provenance (revisit if models multiply).

## 7. Module structure
- `IModel` (in bw_core — unchanged, see D3; no CR needed)
- `firmware/lib/bw_ml/include/bw/ml/`
  - `model_descriptor.h` — `ModelDescriptor {name, model_version, schema_id, schema_version, n_classes, class_names, training_data_hash}` (concrete/generated-header type, app-readable; not on `IModel` — see §13)
  - `model_limits.h` — `kMaxTreeDepth`, `kMaxTrees`, `kMaxModelBytes` (§8); `ml/common/tests/test_export.py` parses this header and fails if the Python mirrors drift
  - `model_validate.h` — shared handshake/`Err` rules used by both families (D3): descriptor → `Err::Corrupt`, vector id/version/count/finiteness → `Err::InvalidArg`, output untouched on `Err`
  - `model_linear.h` / `src/model_linear.cpp` — `LinearSoftmaxModel` (D7); `LinearModelData {weights, bias, feature_count, descriptor}`
  - `model_tree.h` / `src/model_tree.cpp` — `TreeEnsembleModel` (D6); `TreeNode`/`TreeLeaf`/`TreeView` + `kLeafFlag`/`leafRef()`, `TreeModelData {nodes, leaves, trees, node_count, leaf_count, tree_count, feature_count, descriptor}`
  - `feature_schema_v<N>.h` — **GENERATED** schema constants in `namespace bw::ml::schema_v<N>` (N = `schema_id`, so two schema headers can share a TU): `kSchemaId`, `kSchemaVersion`, `kFeatureCount`, `kFeatureNames[]`, `kFeatureMean[]`, `kFeatureStd[]` (identity for `method: none`, D4)
  - `model_<name>_v<N>.h` — **GENERATED** in `namespace bw::ml::<name>` (N = `model_version`): weights/trees + descriptor + ready-to-use `kModel` payload + `kModelBytes` (numeric arrays only) with `static_assert`s for the §8 budget and the layout cross-check
- `ml/common/`
  - `schema.py` — load + validate `feature_schema_v<N>.json` (single source of truth, D2)
  - `export_cpp.py` — deterministic header generation, byte-budget enforcement, model↔schema cross-check, CLI `all`/`check` (D8)
  - `dataset_io.py` — dataset read/write for training (format: `docs/contracts/DATASET_FORMAT.md`) — **deferred, not written**: `DATASET_FORMAT.md` has not landed, so there is no format to read/write yet (see status log)
  - `reference.py` — pure-Python predict for both families + model-artifact loading/validation: the oracle for firmware tests and the exporter (spec §7 originally listed three modules; this one exists so "firmware matches Python" is testable rather than aspirational)
  - `schemas/feature_schema_v<N>.json` — the schema files (committed fixture: `feature_schema_v2.json`, `schema_id` 2 — id 1 belongs to F06, FEATURE_SCHEMA §2.4; the §2.1/§4 worked example)
  - `models/model_<name>_v<N>.json` — model artifacts (weights/trees + embedded metadata) the exporter reads; committed fixtures: `linear_toy`, `tree_toy`, `linear_3class`, `tree_deep`
  - `tests/` — pytest: `conftest.py`, `test_schema.py`, `test_export.py`, `predict_cases.json` (oracle expectations; invoked as `pytest ml/common/tests`: `ml/pyproject.toml` `testpaths=["tests"]` does not cover this dir, and that file is not F05-owned — §13)
- `firmware/test/bw_ml/test_bw_ml/` — Unity host suite: `main.cpp` (same file name as the `bw_activity` suite; strong `setUp`/`tearDown`, registers every test), `test_handshake.cpp`, `test_linear.cpp`, `test_tree.cpp`, `test_contributions.cpp`, `test_no_heap.cpp`, `models_under_test.h` (fixture-model factory + `assertFixtureCase()` helper; **added during Implement** so the case loop exists once), generated `fixture_cases.h`
- `firmware/src/app/wiring/wire_ml.{h,cpp}` — F05's wiring pair, logic-free (01 §1 rule 3); **status: documented no-op (Implement)** — no production `model_<name>_v<N>.h` exists yet (DEC-006: first exported model at M5), so there is nothing to construct. The consumer exists and is waiting: F06's feature assembler has landed, but it reads a hand-maintained mirror of the schema constants (`bw_wake/alertness_schema.h`) instead of F05's generated header, so there is nothing to inject either. The `.cpp` includes the F05 runtime + generated schema header so LDF compiles `bw_ml` under esp32dev. When the first production `model_<name>_v<N>.h` lands, `wire()` gains the model instance + composition-root accessor and the schema-constant injection; the integrator adds the call in `app.cpp`'s fixed order (README — features never edit `app.cpp`)

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kMaxTreeDepth` | 8 | `EXPERIMENTAL` | Bounds flash and latency of exported trees |
| `kMaxTrees` | 32 | `EXPERIMENTAL` | Bounds model size; revisit after F29 results |
| `kMaxModelBytes` | 20480 | `EXPERIMENTAL` | Flash budget §11 as an enforceable limit: checked by `export_cpp.py` at generation and `static_assert`ed on the generated model size; a full depth-8 tree alone is ~511 nodes, so depth/count limits do not imply the byte budget (D6) |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29). Test-side tolerances (Python/firmware parity, probability sum) are themselves named constants in test code, not literals.

## 9. Test plan
**Host — `pio test -e native`, `firmware/test/bw_ml/test_bw_ml/` (Unity):**
- Handshake (D3): wrong `schema_id` → `Err::InvalidArg`; wrong `schema_version` → `Err::InvalidArg`; `count` ≠ expected (0, +1, 255) → `Err::InvalidArg` *before* `v[]` is read (the 255 case would otherwise read out of bounds); NaN/Inf feature → `Err::InvalidArg`; degenerate metadata (0 classes / 0 trees) → `Err::Corrupt`; on any `Err` the output argument is untouched.
- Linear (D7): fixture vectors vs `ml/common/reference.py` oracle, `|Δp| ≤ 1e-5`; `Σp = 1 ± 1e-5`; large-magnitude weights still finite and normalized (max-shift proof); property: increasing a positively weighted feature never lowers that class's probability.
- Contributions (D7): values equal `w·x` of the predicted class per the oracle; feature-index order; cap at `max`; `max == 0` / `nullptr` → `Err::InvalidArg`; bias absent; tree model → `Err::Unsupported`.
- Tree (D6): fixture path parity ≤ 1e-5; soft-voted average equals the oracle; nodes visited ≤ `kMaxTreeDepth + 1`; generated `kModelBytes` ≤ `kMaxModelBytes`.
- Determinism / no heap: two identical `predict()` calls bit-identical (`memcmp`); `t_ms` has no influence on output; global `operator new`/`delete` counters (private helper under `test/bw_ml/` — `bw_testkit` has no allocation hook) show 0 allocations over 1000 `predict()` calls.
- Regeneration staleness (D8): `export_cpp.py` output byte-identical to the checked-in headers (runs under **pytest** — only Python can re-run the exporter; the Unity suite consumes the committed `fixture_cases.h`, whose byte-compare the same test performs).

**Python — `pytest ml/common/tests` + `ruff check ml/common`:**
- Schema validation failures each raise with a field-level message: missing/unknown keys, duplicate feature names, `std ≤ 0`, unknown normalization method, `count > kMaxFeatures`, `schema_id`/`schema_version` = 0.
- Exporter determinism (two runs → identical bytes); constants round-trip bit-exact float32; oracle predictions reproduce the committed fixture expectations.

**Deliberately not covered here:** on-device latency/flash/RAM (Validate stage: `micros()` + `ESP.getMinFreeHeap()`, method and numbers recorded in the status log); F06's end-to-end alertness output (F06's tests); TFLM (§13).

## 10. Failure behavior
- `predict` returns `Err`; caller (F06) reports `valid=false`.
- Never crash on missing/invalid input; return `Err` or an output flagged `Insufficient`.

## 11. Budget
Model ≤ 20 KB flash each `[EXPERIMENTAL]`; `predict` < 1 ms `[EXPERIMENTAL]` at 240 MHz; no heap.

## 12. Out of scope
- Feature assembly (F06).
- Training pipelines for specific tasks (F06/F09).
- On-device training.

## 13. Open questions / assumptions
- Whether TFLM is ever justified (decide from F29).
- `[ASSUMED]` F06 applies the normative normalization formula with the injected generated constants (D4); `FeatureVector::v[]` is training-space. Contract + test vectors live in `docs/contracts/FEATURE_SCHEMA.md`; the integrator verifies F06 complies when it lands.
- `[ASSUMED]` No `IModel` change is needed: enforcement lives inside `predict()` (D3). If a consumer later needs pre-flight `schemaVersion()`/`descriptor()`, that is a change request.
- `[ASSUMED]` `ModelDescriptor` stays a concrete/generated-header type readable by `src/app`; exposing model metadata to features (F22/F23 diagnostics) through `IModel` would be a CR — no consumer exists yet.
- `[ASSUMED]` Python tests live in `ml/common/tests/` and run via `pytest ml/common/tests`; `ml/pyproject.toml` (`testpaths=["tests"]`, mirror-controlled by F00 D13) is not edited — running with an explicit path bypasses `testpaths` without touching a file F05 does not own.
- `[ASSUMED]` Schema ids are allocated by F05 in the registry table of `FEATURE_SCHEMA.md`; id 0 is invalid and never issued.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Schema + exporter documented in `contracts/FEATURE_SCHEMA.md` with a worked example.
- Python code passes `ruff` and `pytest`.

---
