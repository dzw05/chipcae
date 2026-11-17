#include "chipcae/geometry/import_thresholds.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {
void expect(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main() {
    using namespace chipcae::geometry;
    for (const ImportScales scales : std::array{ImportScales{4e-3, 2e-6, 1e-4, 1e-9},
                                                ImportScales{1e-2, 1e-4, 2e-4, 1e-9},
                                                ImportScales{0.1, 1e-3, 5e-4, 1e-9}}) {
        auto result = assess_import(scales);
        expect(static_cast<bool>(result), "in-range scales should be valid");
        const auto& tol = result.value().tolerances;
        expect(tol.eps_weld <= 0.25 * tol.tau_A, "eps_weld ordering violated");
        expect(tol.tau_A <= tol.tau_B, "tau_A ordering violated");
        expect(tol.tau_B >= tol.tau_A, "tau_B should not be below tau_A");
        expect(!result.value().confirmation_required && result.value().ordering_guaranteed,
               "in-range result should guarantee ordering");
    }

    auto outside = assess_import({1e-3, 1e-7, 1e-4, 1e-9});
    expect(static_cast<bool>(outside), "out-of-range scales should be assessable");
    expect(outside.value().tier == Tier::B && outside.value().confirmation_required,
           "out-of-range scales should require tier-B confirmation");
    expect(outside.value().defect &&
               outside.value().defect->reason == DefectReason::TOLERANCE_RANGE_UNSUPPORTED &&
               outside.value().defect->dimension == "L" && outside.value().defect->size == 1e-3 &&
               !outside.value().defect->suggestion.empty(),
           "tier-B defect should describe the violated range");
    auto confirmed = assess_import({1e-3, 1e-7, 1e-4, 1e-9}, {}, true);
    expect(static_cast<bool>(confirmed), "confirmed out-of-range scales should be valid");
    expect(confirmed.value().tolerances.eps_weld == 1e-8 && !confirmed.value().ordering_guaranteed,
           "confirmed result should retain weld floor and flag ordering");
}
