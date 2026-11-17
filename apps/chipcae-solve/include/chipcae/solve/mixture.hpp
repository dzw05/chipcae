#pragma once
#include "chipcae/material/copper_laminate.hpp"
#include "chipcae/solve/case_loader.hpp"

#include <mfem.hpp>
namespace chipcae::solve {
// M7 laminate adapter, with z normal to the stack. C uses engineering
// shear strain (xx, yy, zz, yz, xz, xy), unlike the material-point Mandel API.
struct Constitutive {
    mfem::DenseMatrix C;
    mfem::Vector alpha;
    Constitutive() : C(6), alpha(3) {}
};
inline Constitutive mixture(const Material& matrix, const Material& metal, double f) {
    double p[9]{}, th[3]{};
    material::copper_laminate(
        matrix.E, matrix.nu, metal.E, metal.nu, matrix.alpha, metal.alpha, f, p, th);
    mfem::DenseMatrix compliance(6);
    compliance = 0.0;
    for (int i = 0; i < 3; ++i)
        compliance(i, i) = 1 / p[i];
    compliance(0, 1) = compliance(1, 0) = -p[3] / p[0];
    compliance(0, 2) = compliance(2, 0) = -p[4] / p[0];
    compliance(1, 2) = compliance(2, 1) = -p[5] / p[1];
    compliance(3, 3) = 1 / p[8];
    compliance(4, 4) = 1 / p[7];
    compliance(5, 5) = 1 / p[6];
    Constitutive result;
    mfem::DenseMatrixInverse inverse(compliance);
    inverse.GetInverseMatrix(result.C);
    for (int i = 0; i < 3; ++i)
        result.alpha[i] = th[i];
    return result;
}
} // namespace chipcae::solve
