# CR-001 — OLED controller is SH1106, not SSD1306

- Raised by: integrator (owner) / 2026-09-29
- Type: clarification
- Problem: `docs/00_PROJECT_CONTEXT.md` §3 hardware table states `SSD1306 128×64 over I²C @ 0x3C [ASSUMED]`. The owner has confirmed the module is an **SH1106**. `00` is a shared, locked file, so the correction must go through a change request rather than a direct edit. `04_DECISIONS.md` §2 A-01 (integrator-owned) has already been updated to match.
- Proposal: exact diff to `docs/00_PROJECT_CONTEXT.md` line 41:
  ```diff
  - | 0.96" OLED | Local UI | **SSD1306 128×64 over I²C @ 0x3C [ASSUMED]** |
  + | 0.96" OLED | Local UI | **SH1106 128×64 over I²C @ 0x3C [controller CONFIRMED; address ASSUMED]** |
  ```
- Alternatives considered:
  - Simplest: leave the text and let F01 discover the mismatch during bring-up. Rejected: the doc would silently contradict reality, and `00` instructs contributors to stop when docs conflict.
  - Scalable: correct the locked doc through a CR so the change is traceable. **Chosen.**
- Affected features: F01 (display driver init), F12 (canvas flush). DEC-015 (U8g2) is **unaffected** — U8g2 supports both controllers, only the constructor macro changes (`U8G2_SH1106_128X64_*`).
- Migration / compatibility notes: no code exists yet (F00 not started), so there is nothing to migrate. I²C address `0x3C` remains `[ASSUMED]` pending an I²C scan.
- Status: **accepted** — 2026-09-29, by the developer/integrator. Diff applied to `docs/00_PROJECT_CONTEXT.md` §3 on the same date.
