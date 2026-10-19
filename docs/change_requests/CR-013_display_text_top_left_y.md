# CR-013 — Pin `IDisplayCanvas` text `y` = top-left of the glyph box
- Raised by: F12 / 2026-10-12
- Type: clarification
- Problem: `docs/interfaces/hardware.md` §3 declares `drawText(int16_t x, int16_t y, ...)` without saying what `y` references. The two implementations therefore diverged silently: F01's `U8g2Canvas::drawText` passed `y` straight to `u8g2_.drawStr`, where `y` is the glyph **baseline** (u8g2's default: `u8g2_setup.c` calls `u8g2_SetFontPosBaseline`), while `drawBitmap` uses u8g2's `drawXBM`, which is **top-left** — so one class even carried two different `y` meanings. F12's renderers compose rows assuming `y` = top of the line (the only reading possible with no font metrics on the contract, F12 D7). Bench evidence (developer, 2026-10-12, photos): every screen's row at `y=0` draws its glyph box above the panel edge and is invisible — Home loses the alertness state, Diagnostics loses the `imu:/ppg:` line, Menu loses its title; the alert banner text (baseline ≈ 0–1 px) is clipped by the same bug. This ambiguity was already flagged as an open point in `u8g2_canvas.cpp` and F01's status log ("if F12 assumes top-left, raise a CR").
- Proposal: exact diff to `docs/interfaces/hardware.md` §3:

```diff
 class IDisplayCanvas {                                                                  // 128x64 1-bit; F12 draws, F01 flushes
  public:
   virtual void clear() = 0;
+  // (x, y) is the TOP-LEFT of the glyph box — the font's full box, so no glyph
+  // can be drawn above y — the same reference drawBitmap uses.
   virtual void drawText(int16_t x, int16_t y, const char* text, FontId font) = 0;
+  // (x, y) is the TOP-LEFT of the bitmap.
   virtual void drawBitmap(int16_t x, int16_t y, const uint8_t* xbm, uint8_t w, uint8_t h) = 0;
```

  The same two comment lines belong on the mirror declarations in `firmware/lib/bw_core/include/bw/hardware.h` (locked header — applied with this CR on acceptance).
- Alternatives considered (simplest vs scalable) and why rejected:
  - **Simplest:** pin `y` = baseline (u8g2's native meaning) and fix F12 instead — every renderer adds the current font's ascent to every row (`0+9`, `16+9`, …), and every future consumer must repeat that; the contract would bake a library-specific reference into a library-agnostic interface, and text/bitmap keep two `y` meanings. Rejected: consumers have no font metrics by design (F12 D7), so the conversion would be duplicated, untestable at each call site, and exactly the source of the bench defect.
  - **Scalable:** pin `y` = top-left of the glyph box for all canvas primitives and convert once inside the single implementation that knows the font. The conversion (`baseline = y + font box ascent`) is `U8g2Canvas`'s own arithmetic — u8g2's own metric (`setFontRefHeightAll` → `getFontAscent()`, i.e. `max_char_height + y_offset` = 8/11/16 px for `6x10`/`7x13`/`10x20`, independently verified by decoding the three font binaries) makes it a pure function of the selected font, with no per-font table that can drift.
  - **Choice:** top-left. It is retroactive — the contract never said which semantics apply and F12's existing top-left layouts (goldens) are already correct under it; nothing outside `U8g2Canvas::drawText` changes.
- Affected features: F01 (implements the conversion in `U8g2Canvas::drawText`), F12 (consumer — verified already top-left, no renderer compensated for baseline; layouts/goldens unchanged), F00 (`FakeCanvas` records the consumer-given `y` unchanged; `bw/hardware.h` gains the comment on acceptance).
- Migration / compatibility notes: `grep 'drawText('` shows the only production consumer is `bw_ui/src/screens.cpp` (top-left) — no other call site shifts. F01's `u8g2_canvas.cpp` converts `y` (top-left → baseline) and clamps through the existing `clampPos`, so out-of-range coordinates keep their old draw-nothing behaviour. Bench re-check for the developer: row 1 of every screen visible, banner text on-panel. `docs/interfaces/hardware.md` and `bw/hardware.h` are NOT edited by this CR's branch — they are updated when the CR is accepted.
- Status: accepted 2026-10-12 (integrator, on bench evidence + F12 relaunch review) — contract comments applied to `docs/interfaces/hardware.md` §3 and `firmware/lib/bw_core/include/bw/hardware.h` the same day.
