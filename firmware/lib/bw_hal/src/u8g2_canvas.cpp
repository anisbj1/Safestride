#include <bw/hal/u8g2_canvas.h>

#if defined(ARDUINO)

#include <cstring>

namespace bw::hal {

namespace {
// The one and only canvas: u8x8_t has no user pointer on Arduino builds
// (U8X8_WITH_USER_PTR is defined for unix only, u8x8.h), and the byte
// callback receives nothing but u8x8_t*. One SH1106 on one shared bus is a
// hardware fact (00 §3, CR-001) — u8g2's own ssd13xx cad relies on the same
// single-display assumption (static in_transfer flag in u8x8_cad.c). Set by
// the constructor, before any transfer can occur (wiring order: construct ->
// begin -> tasks).
U8g2Canvas* s_canvas = nullptr;

// FontId -> u8g2 font (02 §7 pins FontId to exactly three values). Metrics
// chosen to fill 128 px: Small 6x10 = 21 chars/line, Medium 7x13 = 18,
// Large 10x20 = 12 — all fit the 64 px height. The "_tf" variants carry
// transparent background, matching IDisplayCanvas' draw-then-flush model.
const uint8_t* fontFor(bw::FontId font) {
    switch (font) {
    case bw::FontId::Medium:
        return u8g2_font_7x13_tf;
    case bw::FontId::Large:
        return u8g2_font_10x20_tf;
    case bw::FontId::Small:
    default:
        return u8g2_font_6x10_tf;
    }
}
}  // namespace

U8g2Canvas::U8g2Canvas(I2cBus& bus, uint8_t addr7, bw::IFaultReporter* faults,
                       const bw::IClock* clock)
    : bus_(bus), u8g2_(U8G2_R0),  // rotation 0, no reset line (U8X8_PIN_NONE default)
      addr7_(addr7), faults_(faults), clock_(clock) {
    s_canvas = this;
    // Install our seam after construction (the U8G2 constructor macro assigns
    // u8x8_byte_arduino_hw_i2c, which would talk to Wire directly — forbidden)
    // and before begin(), which is when u8g2 first uses it.
    u8g2_.getU8x8()->byte_cb = &U8g2Canvas::byteCb;
}

bw::Result<void> U8g2Canvas::begin() {
    // No ID register on this panel: absence = address ACK probe, a zero-length
    // write through the same arbiter every driver uses (spec §9 bench checklist
    // does the same scan).
    const bw::Result<void> probe = bus_.write(addr7_, nullptr, 0);
    if (!probe.ok()) {
        reportDisplayFailed();
        return probe;  // NotFound / bus error — boot continues (spec §10)
    }
    // u8x8 stores the 8-bit write address; the ssd13xx cad applies its 0x78
    // default only while the field is 255 (u8x8_cad.c, CAD_INIT), so ours
    // must be in place before u8g2_.begin() runs the init sequence.
    u8x8_SetI2CAddress(u8g2_.getU8x8(), static_cast<uint8_t>(addr7_ << 1));
    (void)u8g2_.begin();  // returns bool but cannot see transfer failures —
                          // flush() reports those instead
    begun_ = true;
    failed_reported_ = false;
    return bw::Result<void>{bw::Err::Ok};
}

void U8g2Canvas::clear() {
    u8g2_.clearBuffer();
}

void U8g2Canvas::drawText(int16_t x, int16_t y, const char* text, bw::FontId font) {
    u8g2_.setFont(fontFor(font));
    // Resolved by CR-013: IDisplayCanvas::drawText takes y = TOP of the
    // glyph box (the same reference drawBitmap/drawXBM already uses), while
    // u8g2's drawStr wants the BASELINE — convert here, once, where the font
    // is known. Ref-height mode ALL makes u8g2's own ascent metric the full
    // font box (max_char_height + y_offset = 8/11/16 px for the
    // 6x10/7x13/10x20 fonts, verified against the font binaries), i.e. no
    // glyph can draw above it: a row at y=0 lands its glyphs exactly at the
    // panel's top edge instead of above it (bench 2026-10-12: row 1 was
    // invisible with raw baseline semantics). The mode only feeds the
    // ascent/descent metrics — glyph placement in baseline position mode
    // (u8g2_setup.c) uses per-glyph data and is unaffected.
    u8g2_.setFontRefHeightAll();  // (see above; cheap, idempotent, per font)
    const int16_t baseline = static_cast<int16_t>(clampPos(y) + u8g2_.getFontAscent());
    u8g2_.drawStr(clampPos(x), clampPos(baseline), text);
}

void U8g2Canvas::drawBitmap(int16_t x, int16_t y, const uint8_t* xbm, uint8_t w, uint8_t h) {
    u8g2_.drawXBM(clampPos(x), clampPos(y), w, h, xbm);
}

void U8g2Canvas::fillRect(int16_t x, int16_t y, uint8_t w, uint8_t h, bool on) {
    // Draw color 0 erases inside the frame buffer, 1 sets it; the default is
    // restored so every subsequent op keeps the black-on-white model F12
    // composes against.
    u8g2_.setDrawColor(on ? 1 : 0);
    u8g2_.drawBox(clampPos(x), clampPos(y), w, h);
    u8g2_.setDrawColor(1);
}

void U8g2Canvas::setPower(bool on) {
    if (!begun_) {
        return;  // no panel state change before init (and no stray transfer)
    }
    u8g2_.setPowerSave(on ? 0 : 1);
}

void U8g2Canvas::setContrast(uint8_t contrast) {
    if (!begun_) {
        return;
    }
    u8g2_.setContrast(contrast);
}

bw::Result<void> U8g2Canvas::flush() {
    if (!begun_) {
        return bw::Result<void>{bw::Err::NotReady};
    }
    last_err_ = bw::Err::Ok;
    u8g2_.sendBuffer();  // cad -> byteCb -> I2cBus::write (lock taken inside)
    if (last_err_ == bw::Err::Ok) {
        failed_reported_ = false;  // next absent episode reports again (D7 rule)
        return bw::Result<void>{bw::Err::Ok};
    }
    if (last_err_ == bw::Err::NotFound) {
        // Device-level: the panel stopped answering. Timeout/BusError are
        // already reported by I2cBus (D7 edge rule) — do not double-report.
        reportDisplayFailed();
    }
    return bw::Result<void>{last_err_};
}

void U8g2Canvas::reportDisplayFailed() {
    if (failed_reported_ || faults_ == nullptr) {
        return;
    }
    failed_reported_ = true;
    // Warning = the display is unusable but sensing is unaffected (F01's
    // initial mapping; F26 owns the degradation table).
    const bw::TimeMs t_ms = (clock_ != nullptr) ? clock_->nowMs() : 0;
    faults_->report(bw::FaultCode::DisplayFailed, bw::FaultSeverity::Warning, t_ms);
}

u8g2_uint_t U8g2Canvas::clampPos(int16_t v) {
    if (v < 0) {
        return 0;
    }
    // 255 is off-screen for a 128x64 panel: out-of-range coordinates become
    // "draw nothing" instead of wrapping into a valid on-screen position.
    return static_cast<u8g2_uint_t>(v > 255 ? 255 : v);
}

uint8_t U8g2Canvas::byteCb(u8x8_t* u8x8, uint8_t msg, uint8_t arg_int, void* arg_ptr) {
    U8g2Canvas* self = s_canvas;
    if (self == nullptr) {
        return 0;
    }
    switch (msg) {
    case U8X8_MSG_BYTE_INIT:
        // Bus and clock are owned by Esp32I2cBackend (single arbiter,
        // 00 §3): u8g2's default INIT would call Wire.begin()/setClock()
        // behind our back — this is where we decline that.
        return 1;

    case U8X8_MSG_BYTE_SET_DC:
        // I²C has no D/C pin: the ssd13xx cad embeds it as the leading
        // control byte (0x00 command / 0x40 data) inside SEND.
        return 1;

    case U8X8_MSG_BYTE_START_TRANSFER:
        // u8x8 holds the 8-bit write address (0x78 = 0x3C << 1).
        self->tx_addr7_ = static_cast<uint8_t>(u8x8_GetI2CAddress(u8x8) >> 1);
        self->tx_len_ = 0;
        return 1;

    case U8X8_MSG_BYTE_SEND: {
        const uint8_t* data = static_cast<const uint8_t*>(arg_ptr);
        if (data == nullptr || self->tx_overflow_) {
            return 0;
        }
        if (static_cast<size_t>(self->tx_len_) + arg_int > kDisplayTxBufferBytes) {
            // Refuse the whole transfer (Err::Full semantics: nothing is
            // written, the input is dropped) — a cad larger than
            // kDisplayTxBufferBytes would be a library contract change.
            self->tx_overflow_ = true;
            self->last_err_ = bw::Err::Full;
            return 0;
        }
        std::memcpy(&self->tx_buf_[self->tx_len_], data, arg_int);
        self->tx_len_ += arg_int;
        return 1;
    }

    case U8X8_MSG_BYTE_END_TRANSFER: {
        if (self->tx_overflow_) {
            self->tx_overflow_ = false;
            self->tx_len_ = 0;
            return 0;  // already reported as Full by SEND
        }
        if (self->tx_len_ == 0) {
            return 1;  // START..END with no payload: nothing to push
        }
        const bw::Result<void> written =
            self->bus_.write(self->tx_addr7_, self->tx_buf_, self->tx_len_);
        self->tx_len_ = 0;
        self->last_err_ = written.err;
        return written.ok() ? 1 : 0;
    }

    default:
        return 0;
    }
}

}  // namespace bw::hal

#endif  // ARDUINO
