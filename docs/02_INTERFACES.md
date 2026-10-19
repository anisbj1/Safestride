# 02 — INTERFACES (index)

> Shared contracts, split by topic for focused reference. Change contract definitions through a documented change request. Status: DRAFT v0.1. F00 turns these into code in `firmware/lib/bw_core/`; afterwards code + contract tests are authoritative and F00 keeps these files in sync.
> Section numbers (e.g. "02 §6.3") are preserved inside the files.

| File | Sections | Contains |
|---|---|---|
| `interfaces/core.md` | §1, §2, §7 | Conventions, units, `Result`/`Err`, containers, `IClock`, deferred types. **Everyone reads this.** |
| `interfaces/hardware.md` | §3 | Sensor/LED/buzzer/display/battery interfaces, raw samples |
| `interfaces/data_signals.md` | §4.1–4.3 | `DataQuality`, `MotionFeatures`, `PpgFeatures`, `ActivityEstimate`, `WearEstimate` |
| `interfaces/data_decision.md` | §4.4–4.6 | `FeatureVector`, `IModel`, `AlertnessProbabilities`, `StateSnapshot`, `Event`, reason codes, alerts, buttons, `Command` |
| `interfaces/services.md` | §5 | `IStateProvider`, `IEventSink`, `IEventStore`, `IKeyValueStore`, `ICommandSink`, `IFaultReporter`, `IBaselineProvider`, `HealthSnapshot` |
| `interfaces/external.md` | §6 | Time sync, BLE GATT, sync protocol, dataset format, feature schema, change policy |

Your feature spec's **Read** line says which of these to open. Do not read the others.
