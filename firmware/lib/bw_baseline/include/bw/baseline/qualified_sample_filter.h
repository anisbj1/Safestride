#pragma once

// bw/baseline/qualified_sample_filter.h — F13 §7: the pure, table-driven gate
// decision. Stateless apart from its (immutable) config, host-testable, no
// platform calls.

#include <bw/baseline/baseline_config.h>
#include <bw/baseline/baseline_types.h>

namespace bw::baseline {

class QualifiedSampleFilter {
public:
    explicit QualifiedSampleFilter(const BaselineConfig& cfg = BaselineConfig{}) : cfg_(cfg) {}

    // Verdict precedence (documented order, §9 asserts the individual rows):
    //   hold > not-worn > quality > HR plausibility > activity.
    // Hold is first on purpose: a suspected event must be refused even when the
    // sample would fail another gate too (§5 rule 2 — poisoning dominates).
    SampleVerdict check(const BaselineSample& sample) const;

private:
    BaselineConfig cfg_;
};

}  // namespace bw::baseline
