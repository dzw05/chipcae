#pragma once
#include "cut_layer.hpp"
#include "material.hpp"
#include "solvers.hpp"
#include "stubs/k5_stubs.hpp"

#include <functional>
namespace chipcae::proto_cutfem {
enum class BCType { None, Dirichlet, Robin, Traction };
struct BoundaryData {
    BCType type = BCType::None;
    Point value{};
    double hc = 0, Tinf = 0, q = 0;
};
struct Problem {
    int ncomp = 1;
    bool kokkos_volume = false;
    bool kokkos_serial = false;
    bool kokkos_atomic_scatter = false;
    double* volume_seconds = nullptr;
    double gamma_N = 20, gamma_g = .1;
    MaterialParamsSoA materials;
    std::function<Point(Point, int)> source = [](Point, int) { return Point{}; };
    std::function<BoundaryData(int, Point, Point)> boundary = [](int, Point, Point) {
        return BoundaryData{};
    };
    std::function<double(Point)> delta_temperature;
    const chipcae::k5_stub::CvField* temperature = nullptr;
};
struct DofMap {
    int nnode = 0, ncomp = 1;
    std::vector<int> node_to_active, active_to_node;
    int dof(int node, int c = 0) const { return ncomp * node_to_active[node] + c; }
};
struct System {
    CSRMatrix A;
    std::vector<double> rhs;
    DofMap dofs;
};
DofMap active_dofs(const HexMeshArrays&, const CutLayer&, int ncomp);
System assemble(const HexMeshArrays&, const CutLayer&, const Problem&);
double delta_temperature(const Problem&, const HexMeshArrays&, int, const double N[4], Point);
chipcae::k5_stub::CvField
nodal_field(const HexMeshArrays&, const System&, const std::vector<double>&, const std::string&);
chipcae::k5_stub::CvField stress_field(const HexMeshArrays&,
                                       const CutLayer&,
                                       const Problem&,
                                       const chipcae::k5_stub::CvField&);
struct StepRequest {
    const HexMeshArrays& mesh;
    const CutLayer& cut;
    const Problem& problem;
    std::vector<std::pair<int, double>> constraints; // compact DOF indices
    std::string method = "direct";
};
class PrototypeSolver : public chipcae::k5_stub::Solver {
public:
    chipcae::k5_stub::StepResult solve_step(const StepRequest&) override;
};
} // namespace chipcae::proto_cutfem
