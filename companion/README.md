# anarolepsy-companion (F21 + W5 halves)

Local-first **Python desktop companion** for the Anarolepsy bracelet: pairing/bonding,
live status, a read-only synced event timeline, sync control and time sync — plus the
W5 **sync**, **history** and **explain** views (F20/F16/F22). Bleak + SQLite, CLI-first,
no server, no third-party sends.

> **Not a medical device.** The app observes only — the bracelet decides and alerts on
> its own and is *never* required for immediate alerting (docs/00_PROJECT_CONTEXT.md, docs/00 §7.1).
> Detections render as "possible"/"suspected" with a confidence value, never as a
> diagnosis.

## Setup (Python >= 3.11)

```powershell
cd companion
pip install -e .[dev]      # installs bleak + the anarolepsy-app / anarolepsy-bench scripts
```

## Run

```powershell
anarolepsy-bench scan                       # find the bracelet (service UUID + local name)
anarolepsy-bench connect --address <addr>   # prints the negotiated ATT MTU
anarolepsy-bench status  --address <addr>   # device_id / battery / MTU / sync status
anarolepsy-bench sync    --address <addr>   # sync into the local DB (fails closed below MTU 55)

anarolepsy-app watch --once                 # one live-status frame (time sync on connect)
anarolepsy-app timeline                     # read-only synced timeline from the local DB
anarolepsy-app sync    --address <addr>     # sync into the local DB (F20 real client)
anarolepsy-app history                      # sleep/nap history from the local DB (F16)
anarolepsy-app explain --seq <n>            # why an event was raised: reasons + confidence (F22)
anarolepsy-app settime                      # explicit SetTime(unix_seconds)
```

The local DB lives under the OS user data dir (`companion.sqlite3`); override with the
`ANAROLEPSY_COMPANION_DB` env var.

## MTU note (D8, backlog 37)

The device streams sync chunks only while the ATT MTU is **>= 55** (SYNC_PROTOCOL §2);
Event Notify needs >= 51 (BLE_GATT §4.7). The bench requests the device's preferred MTU
(185) on connect and **verifies it by read-back**; `bench sync` refuses below 55 with
`mtu_too_low` and a non-zero exit *before any CURSOR write*. If a backend negotiates
automatically (e.g. WinRT `[ASSUMED]`), the verify step still runs — same fail-closed
behavior either way.

## Bonding (D9)

The bracelet uses Just Works bonding; every notify/indicate CCCD requires an encrypted
link (BLE_GATT §3). On ATT `0x05`/`0x0F` the CLI prints an actionable
"bonding required — pair in the OS Bluetooth settings, then retry" instead of silently
retrying.

## Sync client (D4)

F21 defines the `SyncPort` seam; F20's client lives in `companion/lib/sync/**` and is
the **default** port for `app sync` / `bench sync` (wired via `bound_port_factory`, D12(2))
— cursor-and-ack transfer with contiguity-gated delivery (D9) and a transactional upsert
through F21's single-writer repository. `FakeSyncPort` still drives host tests with zero
BLE hardware.

## Tests

```powershell
cd companion
python -m pytest . -q          # F21 suite + W5 half suites (lib/**/tests), FakeTransport, no BLE hardware
python -m ruff check .         # same rule set as tools/ml (E, F, I, B; line length 100)
```

Golden vectors are the contracts' **own** test vectors (BLE_GATT §6, SYNC §6): drift in
either direction fails the suite.
