#pragma once
#include "cutfem/solvers.hpp"

#include <cstdint>
#include <string>
#include <vector>
namespace chipcae::proto_cutfem {
struct StepRequest;
}
namespace chipcae::k5_stub {
struct Communicator {
    int rank() const { return 0; }
    int size() const { return 1; }
    void barrier() const {}
};
struct CvField {
    enum class Location { Node, Cell, QuadPoint };
    std::string name;
    Location location = Location::Node;
    int ncomp = 1;
    std::vector<double> values;
    std::vector<int64_t> offsets;
};
struct StepResult {
    int status = 0;
    CvField displacement, temperature, stress;
    double warpage = 0, curvature = 0;
    chipcae::proto_cutfem::SolverStats solver_stats;
};
struct Solver {
    virtual ~Solver() = default;
    virtual StepResult solve_step(const chipcae::proto_cutfem::StepRequest&) = 0;
};
} // namespace chipcae::k5_stub
