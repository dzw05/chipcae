#pragma once

#include <functional>
#include <limits>
#include <mfem.hpp>

namespace chipcae::solve {

struct RefineResult {
    double omega = std::numeric_limits<double>::infinity();
    double eta = std::numeric_limits<double>::infinity();
    double rel = std::numeric_limits<double>::infinity();
    int iterations = 0;
    bool accepted = false;
};

// Test-first placeholders only. The CLI does not include or call this header yet.
inline double backward_error_ratio(double, double) {
    return std::numeric_limits<double>::infinity();
}

inline RefineResult refine(const mfem::SparseMatrix&,
                           const mfem::Vector&,
                           mfem::Vector&,
                           const std::function<void(const mfem::Vector&, mfem::Vector&)>&,
                           double,
                           int) {
    return {};
}

} // namespace chipcae::solve
