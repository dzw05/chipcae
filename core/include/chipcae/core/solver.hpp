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

/// Skeleton failure token used by placeholder solvers (PR #5). Prefer ErrorCode
/// and StepResult::Status for new code.
enum class FailureReason { none, not_implemented };

/// Result of a solver step. The first four fields keep the PR #5 aggregate
/// layout used by placeholder FEM/FVM/particle solvers and tests/unit/test_mesh.cpp.
struct StepResult {
    bool success{false};
    FailureReason failure_reason{FailureReason::not_implemented};
    std::size_t iterations{0};
    std::optional<double> suggested_next_step_size;

    enum class Status : std::uint8_t { Converged, CutBack, Failed };
    Status status = Status::Failed;
    int nonlinear_iters = 0;
    int linear_iters = 0;
    double residual = 0.0;
    double dt_next_hint = 0.0;
    ErrorCode code = ErrorCode::INTERNAL;
};

/// IDs are opaque handles resolved by the simulation context. Temperature is
/// constant unless temperature_curve_id is set. shrinkage_increment is delta s_eff.
struct LoadStep {
    double t0 = 0.0;
    double t1 = 0.0;
    double temperature = 0.0;
    std::optional<std::uint64_t> temperature_curve_id;
    double shrinkage_increment = 0.0;
    std::optional<std::uint64_t> activation_mask_id;
};

class CaseNode;
class SimulationContext;

/// Architecture v0.2 Solver contract (K5). Placeholder modules currently
/// override only solve(); the remaining virtuals have empty defaults so those
/// types stay concrete until they are destubbed.
class Solver {
public:
    virtual ~Solver() = default;
    virtual std::string_view name() const { return {}; }
    virtual void configure(const CaseNode&) {}
    virtual void setup(SimulationContext&) {}
    virtual StepResult advance(const LoadStep&, double, double) { return solve(); }
    virtual void checkpoint(io::Writer&) const {}
    virtual void restart(io::Reader&) {}
    virtual void on_regrid() {}
    virtual StepResult solve() = 0;
};
} // namespace chipcae
