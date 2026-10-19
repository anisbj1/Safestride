# CR-026 — Device-initiated ATT_MTU_REQ on connection

- Raised by: F19 / 2026-10-17 (queue-10 closeout, developer-directed)
- Type: clarification (fills an unstated behavior; no interface, payload or schema change)
- Problem: `docs/contracts/BLE_GATT.md` documents the preferred ATT MTU (`kPreferredMtu` = 185)
  and both MTU floors (Event-notify 51, sync engine 55), and `SYNC_PROTOCOL.md` §3/§4.7
  documents `mtu_too_low` — but **neither contract says who sends `ATT_MTU_REQ`**. The
  implementation left it to the central (passive peripheral), and measured reality on this
  bench is that **no tested central ever initiates**: Windows/bleak exposes no request API
  at all (installed bleak WinRT backend has only an `mtu_size` getter reading
  `GattSession.max_pdu_size`; measured `NEGOTIATED_MTU=23`) and the W4 nRF Connect session
  "produced no ATT_MTU_REQ on the device either". The link therefore stays at 23 bytes
  forever, `maxSendBytes()` stays 20 < 52, F20 streams nothing, and `mtu_too_low` is
  permanently set — queue 10 (M4 streaming leg) cannot be closed on any current central.
  Backlog 37's recorded path ("the F21 companion must request MTU") is infeasible on
  Windows as written.
- Proposal (exact wording for `docs/contracts/BLE_GATT.md`, owner F19):
  1. §2 table row becomes:
     `| Preferred ATT MTU | 185 (`kPreferredMtu`, [EXPERIMENTAL]; stack clamps to its max 255 if lower). **The device sends ATT_MTU_REQ itself when a link comes up** (CR-026) — a central that never requests still ends up negotiated; see §4.7 |`
  2. §4.7 (MTU floors paragraph) gains:
     `**Who initiates (CR-026):** the *device* sends ATT_MTU_REQ on connection
     (ConnectionMonitor::onConnect -> ble_gattc_exchange_mtu, preferred = kPreferredMtu).
     The peer's MTU_RSP arrives as BLE_GAP_EVENT_MTU -> onMTUChange -> BleLinkState.mtu
     (CR-018). If the negotiated result stays below a floor, the rows above apply
     unchanged (suppression / mtu_too_low); a failed exchange simply leaves the link at
     23 — still fail-closed, never a fabricated pass. Rationale: no tested central
     initiates (Windows/bleak has no request API, measured 23; nRF Connect session
     measured 23), and F20's streaming floor (55) made waiting on the peer a deadlock
     at 23 for every future PC-side sync.`
- Alternatives considered:
  - *Simplest = status quo, central always initiates* (backlog 37: make F21 request MTU) —
    **rejected**: measured infeasible on Windows (bleak WinRT exposes only a getter), so it
    closes nothing; phone/PC asymmetry would persist for every future central that behaves
    like Windows.
  - *Phone-side manual MTU request per session* (nRF Connect, path a) — **rejected**: needs
    a human and a specific app build every time, closes one bench run rather than the
    behavior class, and the scripted end-to-end verification (`m4_sync_test.py`) still
    cannot run from the PC.
  - *Scalable-but-unneeded = exchange state machine with completion callback + retries* —
    **rejected**: Bluetooth requires every central to answer MTU_RSP (spec-mandated), and
    the only observed failure mode already degrades to today's exactly-tested fail-closed
    state at 23; retry state would have no trigger and no evidence of need.
  - *Chosen* = one fire-and-forget `ble_gattc_exchange_mtu(conn, NULL, NULL)` in the
    existing connect hook (`ConnectionMonitor::onConnect`): the result path
    (`BLE_GAP_EVENT_MTU` → `onMTUChange` → `LinkEvent::MtuChanged` → tracker) is already
    wired and commented ("the MTU exchange follows and lands in onMTUChange"); NULL
    callback is contract-legal inside NimBLE (`ble_gattc_mtu_cb` null-guards it).
- Affected features: **F19** (owner: one glue call in `ConnectionMonitor::onConnect` +
  this contract file), **F20** (beneficiary, zero code — its floor/gate logic is
  untouched), F21 companion + `m4_sync_test.py` (gain a working streaming leg over bleak),
  readers of `BleLinkState.mtu` (F12/F23 — no semantic change).
- Migration / compatibility notes: none required — no payload, layout, field, enum or
  schema-version change; centrals already must answer `ATT_MTU_REQ` (CoreBluetooth,
  Windows, BlueZ all do — any peer that ignores it leaves the link at 23, i.e. exactly the
  current, fully tested floor behavior). Device-initiated exchange is spec-legal from a
  peripheral (ATT allows either side to initiate).
- Status: **accepted + applied 2026-10-17** (integrator, developer-directed queue-10
  closeout; applied on `f19-mtu-init` → `BLE_GATT.md` §2/§4.7 + F19 glue call).
