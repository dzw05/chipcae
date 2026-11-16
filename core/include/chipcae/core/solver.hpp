#pragma once

#include <cstddef>
#include <optional>

namespace chipcae {

enum class FailureReason { none, not_implemented };

struct StepResult {
    bool success{false};
    FailureReason failure_reason{FailureReason::not_implemented};
    std::size_t iterations{0};
    std::optional<double> suggested_next_step_size;
};

class Solver {
public:
    virtual ~Solver() = default;
    virtual void on_regrid() {}
    virtual StepResult solve() = 0;
};

} // namespace chipcae
