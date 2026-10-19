#pragma once

// firmware/src/app/wiring/wire_sync.h — F20's wiring pair (convention:
// src/app/wiring/README.md; the file name wire_sync.* is F20 spec §2 Owns).
//
// Construction, dependency injection and seam binding only — no logic and no
// task (01 §1 rule 3, D6): F20 owns no task, the composition root calls
// pumpOnce() from its existing app service cadence, so there are no
// stack/priority/period constants to declare here.
//
// Exactly three seams (CR-019 reconciliation, G1 ruling 4):
//   1. wire(transport, store)   — ISyncTransport + IEventStore injection point
//   2. pumpOnce(now)            — one engine step (F08 pattern)
//   3. statusProvider()         — bw::ISyncStatusProvider for UI/BLE binding (P4)

#include <bw/providers.h>
#include <bw/sync_transport.h>
#include <bw/types.h>

namespace f20 {

// Injection point: attaches the F19 BLE adapter (ISyncTransport) and the F18
// store (IEventStore) to the static SyncSession. Call once in the README's
// wiring order; nullptr detaches (sync off, no crash). The engine includes
// only bw_core + bw_sync headers — F19's concrete class never appears here
// (01 §1 rule 1); the composition root passes the adapter instance in.
void wire(bw::ISyncTransport* transport, bw::IEventStore* store);

// One bounded pump of the sync engine: drains inbound Control frames, applies
// cumulative acks, streams at most one store batch, flushes a pending STATUS.
// The composition root calls this from its existing cadence (e.g. every few
// seconds while connected); no dedicated task (D6). Never blocks; any error
// leaves lastSyncedSeq untouched (spec §10).
void pumpOnce(bw::TimeMs now);

// Read-only sync status for UI/BLE (CR-019 part B): consumers receive it
// through this bw_core provider only — never through F20 headers.
bw::ISyncStatusProvider& statusProvider();

}  // namespace f20
