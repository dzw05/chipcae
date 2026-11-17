#pragma once

#include "chipcae/core/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace chipcae::geometry {

struct ImportScales {
    Real L = 0;
    // Thinnest layer retained in the mesh after thin-layer merging.
    Real t_min = 0;
    Real h_xy = 0;
    Real dbu_m = 0;
};

struct ThresholdFile {
    Real weld_rel_L = 1e-6;
    Real weld_rel_t = 0.005;
    Real tauA_rel_L = 1e-5;
    Real tauA_rel_t = 0.02;
    Real tauB_rel_L = 1e-4;
    Real tauB_rel_t = 0.1;
    Real merge_rel_h = 0.01;
    Real merge_abs_max = 1e-6;
    Real merge_max_volume_rel = 1e-4;
    Real thin_ratio = 0.1;
    Real thin_aspect = 1000;
    std::uint64_t cell_budget_ecad_laptop = 80000;
    std::uint64_t cell_budget_ecad_server = 200000;
};

struct ImportTolerances {
    Real eps_weld = 0;
    Real tau_A = 0;
    Real tau_B = 0;
    Real delta_f = 0;
};

struct ImportThresholdError {
    std::string message;
};

enum class Tier : std::uint8_t { Weld, A, B, C };
enum class DefectReason : std::uint8_t { TOLERANCE_RANGE_UNSUPPORTED };

struct Defect {
    Tier tier = Tier::B;
    DefectReason reason;
    std::string dimension;
    Real size;
    std::string suggestion;
    std::string defect_id;
    std::string kind;
    std::string action;
    std::string rejection_reason;
    bool material_ambiguous = false;
};

struct ImportAssessment {
    Tier tier = Tier::A;
    ImportTolerances tolerances;
    bool confirmation_required = false;
    bool ordering_guaranteed = true;
    std::optional<Defect> defect;
};

template <class T>
class Expected {
    std::variant<T, ImportThresholdError> value_;

public:
    Expected(T value) : value_(std::move(value)) {}
    Expected(ImportThresholdError error) : value_(std::move(error)) {}
    explicit operator bool() const { return std::holds_alternative<T>(value_); }
    const T& value() const { return std::get<T>(value_); }
    T& value() { return std::get<T>(value_); }
    const ImportThresholdError& error() const { return std::get<ImportThresholdError>(value_); }
};

Expected<ImportTolerances> compute_tolerances(const ImportScales&, const ThresholdFile&);
Expected<ImportAssessment>
assess_import(const ImportScales&, const ThresholdFile& = {}, bool confirmed = false);

Tier classify(Real defect_size, const ImportTolerances&);

enum class ThinLayerResult : std::uint8_t { Keep, MergeSuggested, Error };
ThinLayerResult
classify_thin_layer(Real thickness, Real t_prev, Real t_next, Real h_xy, const ThresholdFile& = {});

} // namespace chipcae::geometry
