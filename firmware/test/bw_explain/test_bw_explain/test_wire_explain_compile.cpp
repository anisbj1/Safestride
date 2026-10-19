// bw_explain — F22 §7 wiring seam: wire_explain.cpp must COMPILE in the host
// build and be a pure pass-through (no logic of its own), so consumers
// (F12/F14/F17) can call app::WireExplain and get byte-identical output to
// bw::explain. The include compiles the wiring TU exactly as the composition
// root will (precedent: bw_ui/test_wire_ui_compile.cpp); nothing is wired into
// app.h/app.cpp this wave — that is the integrator's step.

#include <unity.h>

#include "../../../src/app/wiring/wire_explain.cpp"

namespace {

const bw::ReasonMask kMasks[] = {
    0u,
    1u << 0,
    (1u << 0) | (1u << 17) | (1u << 20),  // known + known + unknown
    0xFFFFFFFFu,                          // worst case: 3 items, tail dropped
};

}  // namespace

void test_wire_explain_render_is_pass_through(void) {
    for (bw::ReasonMask mask : kMasks) {
        char via_wire[bw::explain::kExplainShortBufSize];
        char via_lib[bw::explain::kExplainShortBufSize];
        const size_t wire_n = app::WireExplain::explainShort(mask, via_wire, sizeof via_wire);
        const size_t lib_n = bw::explain::explainShort(mask, via_lib, sizeof via_lib);
        TEST_ASSERT_EQUAL_STRING(via_lib, via_wire);
        TEST_ASSERT_EQUAL_UINT(lib_n, wire_n);
    }
}

void test_wire_explain_reason_text_is_pass_through(void) {
    TEST_ASSERT_EQUAL_STRING(bw::explain::reasonText(bw::ReasonCode::PossibleFall),
                             app::WireExplain::reasonText(bw::ReasonCode::PossibleFall));
    // Out-of-catalog code: both sides agree on "no entry" (never a placeholder).
    TEST_ASSERT_NULL(app::WireExplain::reasonText(static_cast<bw::ReasonCode>(30)));
}

void test_wire_explain_catalog_version_is_reachable(void) {
    TEST_ASSERT_EQUAL_UINT16(1, app::WireExplain::catalogVersion());
}
