#pragma once

// bw/testkit/host_only.h — guard included by every bw_testkit header (F00 spec §7).
//
// bw_testkit is host-only: fakes and parsers ship with tests, never with the
// firmware. Nothing under firmware/src/ may include <bw/testkit/...>, so
// PlatformIO's LDF never builds this library for esp32dev — and if someone breaks
// that rule, this #error fails the device build loudly instead of silently
// shipping a CSV parser (and its flash cost) to the bracelet.

#if defined(ARDUINO)
#error "bw_testkit is host-only (F00 §7): a device source included a <bw/testkit/...> header."
#endif
