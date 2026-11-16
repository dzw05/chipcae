#pragma once
#include "chipcae/core/solver.hpp"
#include <functional>
#include <span>

namespace chipcae::coupling {
/// Both solvers share a mesh/context; mechanical reads the thermal state there.
/// Hooks snapshot/restore BOTH solvers and shared fields at each interval start.
/// CutBack requires hooks because thermal may already have changed shared state.
struct StepLoopPolicy {
    double minimum_dt = 1e-12;
    int maximum_cutbacks = 16;
    std::function<void()> save;
    std::function<void()> restore;
};
StepResult run_steps(Solver& thermal, Solver& mechanical,
                     std::span<const LoadStep> steps, const StepLoopPolicy& policy = {});
}
