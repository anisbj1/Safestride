#pragma once

// bw/hal/u8g2_canvas.h — SH1106 128x64 canvas behind bw::IDisplayCanvas (F01,
// DEC-015 + CR-001). Platform-only file (F01 D5): U8g2 full-frame buffer with
// a custom u8x8 byte callback that routes every transaction through I2cBus —
// u8g2 must never see Wire directly (00 §3: only F01 touches I²C).

#if defined(ARDUINO)

#include <U8g2lib.h>

#include <bw/clock.h>
#include <bw/fault.h>
#include <bw/hardware.h>
#include <bw/result.h>

#include <bw/hal/i2c_bus.h>

namespace bw::hal {

// [DERIVED] Accumulation buffer for one u8x8 START..END transfer. Why 64: the
// ssd13xx "fast" cad sends commands as 1 control byte + 1 command byte + up to
// 31 arg bytes (33 B — u8x8_cad.c "assumes ... not longer than 31 bytes") and
// data as 1 control byte + up to 24 B (25 B). 64 B covers both with margin;
// an overflow is refused (Err::Full), never silently truncated.
constexpr size_t kDisplayTxBufferBytes = 64;

class U8g2Canvas final : public bw::IDisplayCanvas {
public:
    // Pins/address/polarity come from config/hw_pins.h at wiring time.
    // `faults`/`clock` may be nullptr (F01 D7/D9 fallbacks).
    U8g2Canvas(I2cBus& bus, uint8_t addr7, bw::IFaultReporter* faults, const bw::IClock* clock);

    // Address ACK probe (this controller has no ID register — spec §9) then
    // the u8g2 init sequence. Returns the probe error when the display is
    // absent (boot continues, spec §10) and reports DisplayFailed once
    // (Warning — the panel is down, sensing is unaffected, spec §10).
    bw::Result<void> begin();

    // Drawing primitives only touch the RAM frame buffer (no bus traffic) —
    // F12 composes, flush() pushes (02 §3: "F12 draws, F01 flushes").
    void clear() override;
    void drawText(int16_t x, int16_t y, const char* text, bw::FontId font) override;
    void drawBitmap(int16_t x, int16_t y, const uint8_t* xbm, uint8_t w, uint8_t h) override;
    void fillRect(int16_t x, int16_t y, uint8_t w, uint8_t h, bool on) override;

    // Pass-through to u8g2 panel commands; no-ops until begin() succeeded.
    void setPower(bool on) override;
    void setContrast(uint8_t contrast) override;

    // Pushes the frame buffer to the panel; each transfer takes the I2cBus
    // lock internally (02 §3). Err::Ok re-arms the fault latch; NotFound
    // (panel unplugged) reports DisplayFailed once per absent episode.
    // Bus-level failures (Timeout/BusError) are I2cBus' own D7 reports —
    // never double-reported here.
    bw::Result<void> flush() override;

private:
    // u8x8 byte-layer callback (D6 seam): INIT/SEND/START/END accumulate into
    // tx_buf_ and flush through I2cBus::write. Static — u8x8_t has no user
    // pointer on Arduino builds (U8X8_WITH_USER_PTR is unix-only), and the
    // ssd13xx cad itself keeps a static transfer flag for the same
    // single-display reason (u8x8_cad.c).
    static uint8_t byteCb(u8x8_t* u8x8, uint8_t msg, uint8_t arg_int, void* arg_ptr);
    void reportDisplayFailed();
    // u8g2 positions are unsigned (u8g2_uint_t = uint8_t without U8G2_16BIT):
    // clamp into [0, 255] so an out-of-contract coordinate can never wrap to
    // a wrong on-screen position (255 lies outside the 128x64 clip window).
    static u8g2_uint_t clampPos(int16_t v);

    I2cBus& bus_;
    U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2_;  // CR-001: SH1106 constructor
    uint8_t addr7_;
    bw::IFaultReporter* faults_;
    const bw::IClock* clock_;
    uint8_t tx_buf_[kDisplayTxBufferBytes] = {};
    size_t tx_len_ = 0;
    uint8_t tx_addr7_ = 0;
    bw::Err last_err_ = bw::Err::Ok;  // set by byteCb, read by flush()
    bool tx_overflow_ = false;
    bool begun_ = false;
    bool failed_reported_ = false;  // one DisplayFailed per absent episode
};

}  // namespace bw::hal

#endif  // ARDUINO
