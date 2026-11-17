#include "chipcae/geometry/import_thresholds.hpp"

#include <algorithm>
#include <cmath>

namespace chipcae::geometry {
namespace {
bool finite_positive(Real value) {
    return std::isfinite(value) && value > 0;
}

bool valid_thresholds(const ThresholdFile& t) {
    return finite_positive(t.weld_rel_L) && finite_positive(t.weld_rel_t) &&
           finite_positive(t.tauA_rel_L) && finite_positive(t.tauA_rel_t) &&
           finite_positive(t.tauB_rel_L) && finite_positive(t.tauB_rel_t) &&
           finite_positive(t.merge_rel_h) && finite_positive(t.merge_abs_max) &&
           finite_positive(t.merge_max_volume_rel) && finite_positive(t.thin_ratio) &&
           finite_positive(t.thin_aspect) && t.thin_ratio < 1;
}
} // namespace

Expected<ImportTolerances> compute_tolerances(const ImportScales& s, const ThresholdFile& t) {
    if (!finite_positive(s.L) || !finite_positive(s.t_min) || !finite_positive(s.h_xy) ||
        !std::isfinite(s.dbu_m) || s.dbu_m < 0 || !valid_thresholds(t)) {
        return ImportThresholdError{"scales and thresholds must be finite and in range"};
    }

    constexpr Real confusion_m = 1e-9;
    return ImportTolerances{
        std::max(std::min(t.weld_rel_L * s.L, t.weld_rel_t * s.t_min), 10 * confusion_m),
        std::min(t.tauA_rel_L * s.L, t.tauA_rel_t * s.t_min),
        std::min(t.tauB_rel_L * s.L, t.tauB_rel_t * s.t_min),
        std::max(std::min(t.merge_rel_h * s.h_xy, t.merge_abs_max), 2 * s.dbu_m)};
}

Expected<ImportAssessment>
assess_import(const ImportScales& s, const ThresholdFile& t, bool confirmed) {
    auto computed = compute_tolerances(s, t);
    if (!computed)
        return computed.error();

    constexpr Real min_length = 4e-3;
    constexpr Real min_kept_thickness = 2e-6;
    const bool length_unsupported = s.L < min_length;
    const bool thickness_unsupported = s.t_min < min_kept_thickness;
    if (!length_unsupported && !thickness_unsupported)
        return ImportAssessment{Tier::A, computed.value(), false, true, std::nullopt};

    const std::string dimension = length_unsupported ? "L" : "t_min";
    const Real size = length_unsupported ? s.L : s.t_min;
    ImportAssessment assessment{Tier::B,
                                computed.value(),
                                true,
                                false,
                                Defect{Tier::B,
                                       DefectReason::TOLERANCE_RANGE_UNSUPPORTED,
                                       dimension,
                                       size,
                                       "merge into an adjacent layer or use an equivalent layer"}};
    if (confirmed) {
        assessment.confirmation_required = false;
        assessment.tolerances.eps_weld = 1e-8;
    }
    return assessment;
}

Tier classify(Real defect_size, const ImportTolerances& tolerances) {
    if (!std::isfinite(defect_size) || defect_size < 0 || !std::isfinite(tolerances.eps_weld) ||
        !std::isfinite(tolerances.tau_A) || !std::isfinite(tolerances.tau_B)) {
        return Tier::C;
    }
    if (defect_size <= tolerances.eps_weld)
        return Tier::Weld;
    if (defect_size <= tolerances.tau_A)
        return Tier::A;
    if (defect_size <= tolerances.tau_B)
        return Tier::B;
    return Tier::C;
}

ThinLayerResult classify_thin_layer(
    Real thickness, Real t_prev, Real t_next, Real h_xy, const ThresholdFile& thresholds) {
    if (!std::isfinite(thickness) || !std::isfinite(t_prev) || !std::isfinite(t_next) ||
        !std::isfinite(h_xy) || thickness < 0 || t_prev <= 0 || t_next <= 0 || h_xy <= 0 ||
        !valid_thresholds(thresholds)) {
        return ThinLayerResult::Error;
    }
    if (thickness == 0)
        return ThinLayerResult::Error;
    if (thickness < thresholds.thin_ratio * std::min(t_prev, t_next) ||
        h_xy / thickness > thresholds.thin_aspect) {
        return ThinLayerResult::MergeSuggested;
    }
    return ThinLayerResult::Keep;
}

} // namespace chipcae::geometry
