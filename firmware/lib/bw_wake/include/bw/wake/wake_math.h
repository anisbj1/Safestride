#pragma once

// bw/wake/wake_math.h — the numeric helper every F06 module needs, kept in
// one place so no threshold ever hides inside an algorithm (00 §7.5).

namespace bw::wake {

// Clamps to [0,1]. NaN maps to 0 (no evidence): the comparisons are written
// so a corrupt float degrades to "nothing observed" instead of propagating a
// NaN into a probability (00 §1: never fabricate from bad data).
inline float clamp01(float v) {
    if (!(v > 0.0f))
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

}  // namespace bw::wake
