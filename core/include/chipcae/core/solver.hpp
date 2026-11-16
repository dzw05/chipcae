#pragma once

#include "chipcae/core/error_code.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace chipcae {
namespace io {
class Writer;
class Reader;
} // namespace io

struct StepResult {
    enum class Status : std::uint8_t { Converged, CutBack, Failed };
    Status status = Status::Failed;
    ErrorCode code = ErrorCode::INTERNAL; // Converged results must use NONE.
    int nonlinear_iters = 0;
    int linear_iters = 0;
    double residual = 0.0;
    double dt_next_hint = 0.0; // <= 0 means no suggestion.
};

/// One (sub-)load step over the interval [t0, t1].
/// temperature_t0 and temperature are T(t0) and T(t1), respectively; both
/// have the same optional type and are empty when there is no thermal load.
/// shrinkage_increment is Δs_eff over this interval and scales by dt/(t1-t0)
/// when the caller subdivides the step. activation_mask_id applies only at the
/// interval start.
struct LoadStep {
    double t0 = 0.0;
    double t1 = 0.0;
    std::optional<double> temperature_t0;
    std::optional<double> temperature;
    std::optional<std::uint64_t> temperature_curve_id;
    double shrinkage_increment = 0.0;
    std::optional<std::uint64_t> activation_mask_id;
};

class CaseNode;
class SimulationContext;

class Solver {
public:
    virtual ~Solver() = default;
    virtual std::string_view name() const = 0;
    virtual void configure(const CaseNode&) {}
    virtual void setup(SimulationContext&) {}
    /// The sole step entry point; the interval is given by step.t0/t1. Failures
    /// are reported through status/code rather than exceptions.
    virtual StepResult advance(const LoadStep& step) = 0;
    virtual void checkpoint(io::Writer&) const;
    virtual void restart(io::Reader&);
    virtual bool supports_checkpoint() const { return false; }
    virtual void on_regrid() {}
};
} // namespace chipcae
