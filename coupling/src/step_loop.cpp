#include "chipcae/coupling/step_loop.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace chipcae::coupling {
namespace {
StepResult failure() {
    StepResult r;
    r.status = StepResult::Status::Failed;
    r.code = ErrorCode::INTERNAL;
    return r;
}
} // namespace
StepResult run_steps(Solver& thermal,
                     Solver& mechanical,
                     std::span<const LoadStep> steps,
                     const StepLoopPolicy& policy) {
    if (!(policy.minimum_dt > 0) || !std::isfinite(policy.minimum_dt) ||
        policy.maximum_cutbacks < 0 ||
        static_cast<bool>(policy.save) != static_cast<bool>(policy.restore))
        throw std::invalid_argument("invalid step loop policy");
    for (const auto& step : steps)
        if (!std::isfinite(step.t0) || !std::isfinite(step.t1) || step.t1 <= step.t0 ||
            !std::isfinite(step.t1 - step.t0))
            throw std::invalid_argument("invalid load step interval");
    StepResult result;
    result.status = StepResult::Status::Converged;
    result.code = ErrorCode::NONE;
    for (const auto& step : steps) {
        double t = step.t0, dt = step.t1 - step.t0;
        const double span = step.t1 - step.t0;
        const auto start_temperature = step.temperature_t0 ? step.temperature_t0 : step.temperature;
        const auto end_temperature = step.temperature ? step.temperature : step.temperature_t0;
        auto previous_temperature = start_temperature;
        double shrink_applied = 0;
        while (t < step.t1) {
            dt = std::min(dt, step.t1 - t);
            if (policy.save)
                policy.save();
            int retries = 0;
            for (;;) {
                if (t + dt == t)
                    return failure();
                auto sub = step;
                sub.t0 = t;
                // Snap when the remainder is within a relative slop of dt so a
                // 1 ulp leftover cannot become an extra near-zero substep.
                const double remaining = step.t1 - t;
                sub.t1 = (remaining <= dt * (1.0 + 1e-9)) ? step.t1 : t + dt;
                // The last substep takes the leftover so the increments sum to
                // the parent value within a rounding unit, not ~N ulps.
                sub.shrinkage_increment = (sub.t1 == step.t1)
                                              ? step.shrinkage_increment - shrink_applied
                                              : step.shrinkage_increment * (sub.t1 - sub.t0) / span;
                if (t > step.t0)
                    sub.activation_mask_id.reset();
                if (step.temperature_curve_id) {
                    sub.temperature_t0.reset();
                    sub.temperature.reset();
                } else if (start_temperature) {
                    sub.temperature_t0 = previous_temperature;
                    sub.temperature = *start_temperature + (*end_temperature - *start_temperature) *
                                                               (sub.t1 - step.t0) / span;
                }
                result = thermal.advance(sub);
                if (result.status == StepResult::Status::Converged)
                    result = mechanical.advance(sub);
                if (result.status == StepResult::Status::Converged) {
                    previous_temperature = sub.temperature;
                    shrink_applied += sub.shrinkage_increment;
                    t = sub.t1;
                    break;
                }
                if (policy.restore)
                    policy.restore();
                if (result.status == StepResult::Status::Failed)
                    return result;
                if (!policy.restore || retries++ >= policy.maximum_cutbacks)
                    return failure();
                const double hint = result.dt_next_hint;
                dt = std::isfinite(hint) && hint > 0 && hint < dt ? hint : dt * 0.5;
                if (dt < policy.minimum_dt)
                    return failure();
            }
        }
    }
    return result;
}
} // namespace chipcae::coupling
