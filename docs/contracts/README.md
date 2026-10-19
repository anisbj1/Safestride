# docs/contracts/ — cross-feature protocol & data contracts

Every file in this directory is a **contract**: a format or protocol that two or more
features must agree on byte-for-byte. Contracts are the one part of the documentation
that code is allowed to be *checked against* — the firmware test suite
(`firmware/test/bw_core/`) pins the values that mirror these files.

## Index and owners

The authoritative index lives in `docs/02_INTERFACES.md` §6 (do not duplicate it here;
that section is changed only through a change request). Current files:

| File         | Contract                              | Owner |
|--------------|---------------------------------------|-------|
| `SPEC.md`    | Project specification                 | —     |
| `BLE_GATT.md`       | BLE GATT services & characteristics | F19   |
| `SYNC_PROTOCOL.md`  | Bracelet ↔ phone sync protocol     | F20   |
| `DATASET_FORMAT.md` | Session capture / dataset format   | F28   |
| `FEATURE_SCHEMA.md` | ML feature vector schema           | F05   |
| `CAM_LINK.md`       | ESP32-CAM link protocol            | F27   |

## Rules

1. **A contract file has exactly one owning feature.** Only the owner edits it; other
   features consume it read-only. If you need a change to a contract you do not own,
   write a change request and update the relevant contract documentation instead of editing it.
2. **Append-only evolution.** Enum values, characteristic UUIDs, field names and record
   types are never renumbered or renamed in place — only added. Consumers pin the old
   values, so breaking one silently breaks every feature downstream.
3. **Contracts are versioned where they are consumed.** The consuming code (and its
   test) carries the version; bumping a contract without bumping the consumer's
   expectation is a defect.
4. **The code and the contract must be tested together.** When a contract states a
   value (a UUID, a size, an enum number), the owning feature's tests assert the same
   value so drift is caught by `ci/check.py`, not in the field.
5. **Unknown or ambiguous contract text is a stop condition.** Do not guess at intent —
   raise a change request or ask.
