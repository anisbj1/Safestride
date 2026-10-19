# F25 — Power management

> Track **E** · Milestone **M6** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Reduce energy use after functional correctness, via explicit power profiles: display timeout, sensor duty/LED current, BLE parameters, light sleep, dynamic sampling per profile.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `PowerProfile`, `OperatingMode`
- `BatteryReading`
- Hooks: `IPowerControllable` (implemented by HAL wrappers in wiring, not in F01 code)

**Outputs (produced contracts)**
- Power settings applied via `IPowerControllable`

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Optimize before a measurement exists.
- Reduce alert reliability.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Strategy**
- Simplest: Display timeout + slower BLE advertising.
- Professional/scalable: Tickless idle/light sleep, PPG duty-cycling, adaptive sampling.
- Trade-offs: Advanced methods complicate timing and can hurt signal quality.
- **Choice:** simple measures first (display timeout, LED current, advertising interval); advanced techniques only if a concrete need appears.

## 7. Module structure
- `PowerPolicy`
- `IPowerControllable`

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Policy logic on host (profile → settings).

## 10. Failure behavior
- Unknown battery ⇒ Normal profile.

## 11. Budget
Negligible RAM.

## 12. Out of scope
- Hardware changes.

## 13. Open questions / assumptions

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Profile → settings table documented and covered by host tests.

---
