# F12 — OLED user interface

> Track **C** · Milestone **M2** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Minimalist local UI: status, alerts and essential controls. Pure rendering logic producing draw operations on `IDisplayCanvas`; screens: Home, Alert banner (highest priority), Confirm prompt, minimal Menu, Diagnostics.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `StateSnapshot`, `AlertStatus`, `PpgFeatures` (HR), `BatteryReading`, `HealthSnapshot`
- Confirm-prompt requests (`ConfirmPromptState` provider from F14 wiring)
- `Command{NextScreen, OpenMenu}`
- `IClock`

**Outputs (produced contracts)**
- Draw calls on `IDisplayCanvas`
- `Command` requests (menu selections) via `ICommandSink`

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Include a graphics library header.
- Read sensors or storage.
- Redraw faster than needed (≤ 10 fps).

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Rendering**
- Simplest: Draw text directly in the task.
- Professional/scalable: `UiModel` (data) → `ScreenRenderer` (draw ops) → canvas, screens as small classes.
- Trade-offs: More files but host-testable with `FakeCanvas`.
- **Choice:** Model/renderer split.

**D2. Screen count**
- Simplest: Many pages.
- Professional/scalable: 3–4 screens.
- Trade-offs: Fewer screens keep it usable when drowsy.
- **Choice:** Home / Alert / Prompt / Diagnostics + tiny menu.

**D3. Alert is an overlay, not a screen (W2)**
- Problem: the alert banner must have highest priority (§1) and wake the screen, but the user's screen context (Home / Menu / Diagnostics) must survive the alert.
- Simplest: `AlertScreen` is one more entry in the screen cycle; a non-normal `AlertStatus` switches to it.
- Professional/scalable: alert is a transient OVERLAY (00 §6: "alert is a transient overlay, not a mode") drawn on top of whatever screen is active; min-show time prevents flicker; when the level returns to `Normal` the underlying screen reappears.
- Trade-offs: overlay needs a two-pass draw (screen, then banner band) and min-show bookkeeping; the screen-cycle option loses the user's place and cannot show e.g. Menu+banner.
- **Choice:** Overlay. `ScreenManager` tracks the active `ScreenId` plus `banner_until_ms`; any `AlertStatus.level >= Warning` raises the banner, sets `banner_until_ms = now + kAlertScreenMinShowMs`, wakes the display, and the banner keeps rendering until both the level is `Normal` and `now >= banner_until_ms`.

**D5. Data access from pure UI logic (HealthSnapshot provider does not exist yet; F23 lands it in this wave)**
- Problem: the model needs `StateSnapshot`, `AlertStatus`, `PpgFeatures`, `HealthSnapshot`, `BatteryReading`, time — but must compile and be testable on the host, and `ctx.health` will not exist when F12 is written.
- Simplest: pass concrete pointers to `LatestValue` mailboxes and F01 accessors straight into the model.
- Professional/scalable: define a feature-local `IUiDataSource` port and one wiring adapter.
- Trade-offs: mailboxes are already `bw_core` and host-safe; a port+adapter adds a layer with no contract gain (there is no second data source to abstract).
- **Choice:** a plain `bw::ui::UiInputs` struct of `bw_core` pointers: `const bw::LatestValue<bw::StateSnapshot>* state`, `const bw::LatestValue<bw::AlertStatus>* alert`, `const bw::LatestValue<bw::PpgFeatures>* ppg`, `const bw::LatestValue<bw::HealthSnapshot>* health` (nullptr until F23 wiring lands), `bw::IBatteryMonitor* battery`, `bw::IClock* clock` (nullptr-safe: with no clock, timeouts/staleness are disabled and all ages read 0). Every pointer may be null; the model renders "unknown" honestly per field. `wire_ui.cpp` attaches the ctx mailboxes with the same SFINAE detection idiom `wire_input.cpp` uses, so it compiles before/after F23's fields exist. Host tests fill the struct with real `LatestValue` mailboxes — no fakes needed for the mailboxes themselves.

**D7. Text layout and truncation**
- Problem: `IDisplayCanvas` gives no font metrics; long strings must not wrap or write out of the 128×64 area.
- Simplest: hard character caps per font, no wrapping.
- Professional/scalable: per-font glyph-width table + wrapping.
- Trade-offs: glyph widths live in F01's renderer (F12 may not include its headers); a width table would duplicate F01 data and drift.
- **Choice:** conservative per-font character caps (`kMaxLineCharsSmall/Medium/Large`, `[DERIVED]` from 128 px / ~6 px-per-glyph Small, verified at bench in HIL). A `text_layout` helper copies through `snprintf` into fixed `std::array<char, N>` line buffers, truncating with no NUL-overflow; no wrapping in v1 (out of scope §12).

**D9. Text `y` reference on `IDisplayCanvas` (bench-forced contract pin, CR-013)**
- Problem: `hardware.md §3` never said whether `drawText`'s `y` is the glyph TOP or the BASELINE. F12 composes every row top-left (the only possible reading — the contract deliberately carries no font metrics, D7), but F01's `U8g2Canvas::drawText` passed `y` straight to u8g2's `drawStr`, which reads it as the baseline — while `drawBitmap` was top-left all along, i.e. two `y` meanings in one interface. Bench (developer, 2026-10-12): every row at `y=0` drew above the panel edge and was invisible (Home lost the alertness state, Diagnostics the `imu:/ppg:` line, Menu its title); the banner text was clipped by the same bug.
- Simplest: keep u8g2 baseline semantics as the contract meaning and teach every renderer to add the current font's ascent to every row — F12 has no metrics, so each current and future consumer would duplicate a per-font table, and text/bitmap would keep two `y` references.
- Professional/scalable: pin `y` = top-left of the glyph box for the whole canvas (same reference as `drawBitmap`) and convert once inside the single implementation that knows the font — one reference everywhere, fakes record exactly what consumers composed, no consumer needs metrics.
- Trade-offs: baseline keeps u8g2's native math (nothing to convert, nothing to get wrong) but bakes a library detail into a library-agnostic contract and repeats the conversion at every call site; top-left adds one conversion in F01 — `baseline = y + ascent`, ascent from u8g2's own metric (`setFontRefHeightAll()` → `getFontAscent()` = `max_char_height + y_offset` = 8/11/16 px for `6x10`/`7x13`/`10x20`, independently verified by decoding the three font binaries — no table that can drift) — and retroactively matches F12's already golden-tested layouts (non-banner goldens byte-identical after the pin).
- **Choice:** top-left, recorded as **CR-013** (clarification: the contract was ambiguous, hardware evidence forced the choice). F01's `U8g2Canvas::drawText` converts; F12 render code stays as written. Same session also fixes the alert banner (backlog 21): white fill dropped, the band is masked by erasing it, icon + level text draw normally, a 1 px rule closes the band — no inversion op added to the contract.

## 7. Module structure
### D10. Opt-in demo full-screen prompts (2026-10-19)
- Problem: underlying home/menu content distracts from orange/red demo cues.
- Simplest: replace the standard band everywhere. Rejected: changes the normal
  UI and its min-show behavior beyond this demo request.
- Scalable: configurable overlay presentation. Choice: `UiConfig.demo_wake_protocol`
  defaults false, enabled in wire_ui; the demo overlay masks the entire screen
  without changing the saved screen/menu selection. No base-screen draw calls
  occur on either blink half. On response/safety stop it clears immediately;
  the standard overlay and all its goldens remain unchanged.
- `kDemoBannerBlinkHalfPeriodMs=500` [EXPERIMENTAL] gives 1 Hz blinking, starting
  on at each level's device timestamp. Large-font text is centred from fixed
  10x20 font geometry (`kDemoBannerGlyphHeightPx=20` [HW], F01 font mapping).
  Warning says Drowsy?, Active/red says Sleeping?; prompts describe the demo
  protocol only, not inferred or diagnosed sleep. No new shared UI contract.

```
firmware/lib/bw_ui/
  include/bw/ui/ui_config.h       UiConfig (per-instance knobs) + kUi* layout constants
  include/bw/ui/ui_model.h        UiInputs (D5), UiFrame, UiModel::step(const UiInputs&, TimeMs)
  include/bw/ui/screen_manager.h  ScreenId, banner overlay (D3), handleCommand, takeAction
  include/bw/ui/screens.h         HomeScreen / MenuScreen / DiagnosticsScreen / PromptScreen render fns
  include/bw/ui/text_layout.h     truncating line formatter (D7)
  include/bw/ui/icons.h           XBM arrays (battery, warning triangle)
  src/*.cpp                       one per header
firmware/src/app/wiring/wire_ui.h/.cpp   f12::wire(ctx), f12::uiTask, uiCommands(), task constants
firmware/test/bw_ui/test_bw_ui/          Unity suites + main.cpp + golden/ (§14)
```
- `UiModel::step` produces a `UiFrame { bool power; ScreenId screen; BannerState banner; HomeData home; DiagData diag; MenuData menu; }` — pure, no canvas, no platform calls.
- Screen render functions are free functions `render(const UiFrame&, bw::IDisplayCanvas&)` (small classes were considered; functions keep them stateless and golden-testable — D1's "small classes" satisfied at equal simplicity).
- `ScreenManager::takeAction()` returns the menu's semantic action (`MenuAction` enum, feature-local); the wiring converts it to `Command` only when a consumer exists (see §15 step 8).

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kUiFrameIntervalMs` | 100 | `EXPERIMENTAL` | 10 fps is plenty for status text; less bus load (spec §5 cap) |
| `kDisplayTimeoutMs` | 15000 | `USER` | Screen off after inactivity (power); alerts wake it. `[USER]`-intended; until F18 registers a SettingKey it is a compile-time default |
| `kAlertScreenMinShowMs` | 3000 | `EXPERIMENTAL` | Avoid flicker of the banner |
| `kUiTaskPeriodMs` | 100 | `DERIVED` | = kUiFrameIntervalMs: exactly one frame per tick, so the ≤10 fps cap holds by construction |
| `kUiTaskStackBytes` | 3072 | `EXPERIMENTAL` | Model frame + two snprintf line buffers + render chain; re-measure with uxTaskGetStackHighWaterMark at DoD |
| `kUiTaskPriority` | 2 | `DERIVED` | ≤3 per W2 convention; below InputTask 3 (hard debounce timing) and F10's high-priority alert task, above idle — a late frame is cosmetic, a late button tick is not |
| `kUiTaskCore` | 1 | `DERIVED` | runtime.md §3.2: UiTask on core 1; mailbox reads are single-word anyway, one core avoids cross-core traffic |
| `kUiCommandQueueLen` | 8 | `DERIVED` | UI commands are human-rate (a few/s worst case); 8 holds seconds of backlog; full queue = counted drops, never a crash |
| `kHrStaleMs` | 10000 | `EXPERIMENTAL` | PpgFeatures are 1 Hz (§4.2); 10 s without a new valid frame ⇒ show "--" rather than a stale number |
| `kGlyphWidthSmallPx` | 6 | `ASSUMED` | F01 renderer glyph advance (§13); named so the char caps and their `static_assert`s are arithmetic instead of magic numbers (audit F-2); verify at bench, adjust the constant only |
| `kGlyphWidthMediumPx` | 8 | `ASSUMED` | Same, Medium font advance |
| `kGlyphWidthLargePx` | 10 | `ASSUMED` | Same, Large font advance |
| `kMaxLineCharsSmall` | 21 | `DERIVED` | 128 px / kGlyphWidthSmallPx; conservative cap, verify at bench |
| `kMaxLineCharsMedium` | 16 | `DERIVED` | 128 px / kGlyphWidthMediumPx |
| `kMaxLineCharsLarge` | 12 | `DERIVED` | 128 px / kGlyphWidthLargePx |
| `kMaxLineCharsBanner` | 13 | `DERIVED` | (128 − `kBannerTextXPx` 18) / kGlyphWidthMediumPx: the banner text starts after the icon, so it needs a cap tighter than the plain Medium 16 (audit F-2); pinned by `static_assert` "full-cap line at its origin ≤ 128 px" |
| `kMaxLineCharsMenuText` | 19 | `DERIVED` | (128 − `kMenuTextXPx` 10) / kGlyphWidthSmallPx: menu entries start after the ">" cursor, tighter than the plain Small 21 (audit F-2); same `static_assert` pin |
| `kFlushFailFaultThreshold` | 20 | `EXPERIMENTAL` | 20 consecutive failed flushes (~2 s at 10 fps) before one IFaultReporter report; transient I²C hiccups retry silently, persistent failure is reported once, not per frame |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
Host suite `firmware/test/bw_ui/test_bw_ui/` (`pio test -e native -f "*bw_ui*"` — PlatformIO glob-matches the filter against suite paths, so the bare `-f bw_ui` matches nothing: it collects all 13 suites and runs 0 cases; the wildcard form was verified on the Windows host: 66/66 PASSED), Unity, `bw_testkit::FakeClock` + `bw_testkit::FakeCanvas` (text-log goldens, `setFlushResult` for fault injection). Golden strategy per §14: one `.log` file per screen/state under `test/bw_ui/golden/`, compared as strings; diffs reviewed by eye (human-readable logs beat framebuffers — fake_canvas.h rationale).

| File | Covers |
|---|---|
| `test_screen_manager.cpp` | NextScreen cycle Home→Diagnostics→Home (Prompt skipped while unavailable, D6); OpenMenu opens menu, menu back returns; direct Prompt activation renders "No prompts"; empty command queue = no change (first-call state) |
| `test_alert_overlay.cpp` | Warning raises banner within one frame over Home AND over Menu (priority); banner persists ≥ kAlertScreenMinShowMs after return to Normal (timeline, FakeClock); Persistent level keeps banner; acknowledged flag renders "ACK"; alert wakes display from timed-out state |
| `test_home_screen.cpp` | Goldens: Awake/Worn, Drowsy, NotWorn, Unknown, no-snapshot-yet (startup); HR 72.0 valid → "72"; quality not Good (Insufficient weak-perfusion / Degraded motion) → the SAME plain "72"/"154" (2026-10-15 reversal; `~` absence pinned so a reintroduced marker fails); hr_valid=false (bench: MAX30102 absent) → "--"; stale PpgFeatures → "--"; battery valid → "87%"; battery valid=false (bench A-04) → "--" |
| `test_diagnostics_screen.cpp` | Goldens with a hand-built HealthSnapshot fake (contract type from bw_core; F23 not depended on): quality Degraded shown, fault_bits, min_free_heap, ble_connected false; health == nullptr → "n/a" lines (W2 default) |
| `test_prompt_screen.cpp` | Degraded placeholder golden; cycle skip; availability flip re-enters cycle without other changes |
| `test_timeout.cpp` | FakeClock timeline: frames stop → kDisplayTimeoutMs → power off; command resets timer; alert raises → power on + timer reset |
| `test_flush_failure.cpp` | setFlushResult(BusError): frame retried next tick (no crash, no state loss), failure counter rises, exactly one fault report at kFlushFailFaultThreshold, recovery to Ok resumes normal frames |
| `test_text_layout.cpp` | Truncation exactly at cap, longer string truncated NUL-safe, empty string, `%`-chars in input never interpreted |
| `test_golden.cpp` | The §14 golden set: one canonical frame per screen, byte-compared to `golden/*.log` |
| `test_no_heap.cpp` | Allocation-hook: N× `UiModel::step` + render in steady state allocates 0 times (pattern: bw_ml/test_no_heap.cpp) |
| `test_wire_ui_compile.cpp` | wire_ui.cpp compiles in the host build; D4 port `uiCommands()` accepts pushes; wire() binds a program-lifetime ctx and never leaves dangling pointers, and a rewire through a second ctx is what the model then reads (audit Low-2) |

## 11. Budget
RAM < 2 KB (screen state: UiModel frame + ScreenManager + line buffers) — framebuffer lives in F01. UiTask stack 3 KB (`kUiTaskStackBytes`), measured at DoD. CPU: one frame ≤ 10 Hz; draw ops are µs-scale, the I²C flush is F01's; steady-state UiTask well under 5 % of core 1 — measure at DoD.

## 12. Out of scope
- Fancy animation, settings editor beyond a tiny menu, text wrapping, non-English text.

## 13. Open questions / assumptions
- Language/units: English-only v1 `[ASSUMED]`.
- Glyph widths for the char caps are F01 renderer facts `[ASSUMED]` (~6/8/10 px); verify at bench (HIL checklist), adjust constants only.
- `ctx.health` (F23) may not exist when F12 lands: SFINAE-optional attach, nullptr path tested `[ASSUMED]`.
- Menu selections dispatch no `Command` in W2 (no consumer exists yet: AcknowledgeAlert→F10 pump deferred, MarkEvent→W3); semantic `MenuAction` is the tested port `[ASSUMED]`.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Golden images/logs for each screen stored under `test/bw_ui/golden/`.

---
