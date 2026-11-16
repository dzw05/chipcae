#pragma once
#include "assembly.hpp"

#include <string>
namespace chipcae::proto_cutfem {
struct Options {
    std::string output = "results";
    double gamma_N = 20, gamma_g = .1;
    int N = 0;
    std::string solver = "direct";
    bool gp_specified = false;
    bool dump_matrices = false;
    CutIntegrationOptions quadrature;
};
MaterialParamsSoA mms_material();
Body mms_body();
Point exact_thermal_gradient(Point);
double exact_thermal(Point);
Point exact_elastic(Point);
void exact_elastic_gradient(Point, double grad[2][2]);
double mms_dT(Point);
Point elastic_force(Point, const MaterialParamsSoA&, int = 0);
Point elastic_traction(Point, Point, const MaterialParamsSoA&, int = 0);
void run_mms(bool elastic, bool bimat, const Options&);
void run_timoshenko(const Options&);
void run_gp_sweep(const Options&);
int run_unit_tests();
} // namespace chipcae::proto_cutfem
