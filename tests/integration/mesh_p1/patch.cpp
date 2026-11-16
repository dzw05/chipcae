#include "chipcae/mesh/background/mfem_adapter.hpp"
#include "support.hpp"

#include <cstdlib>
#include <iostream>
#include <mfem.hpp>
using namespace chipcae::mesh;
using k1::expect;
namespace {
constexpr double gradient[3][3] = {{.1, .2, -.1}, {.05, -.15, .1}, {.2, .05, .3}};
void exact(const mfem::Vector& x, mfem::Vector& u) {
    u.SetSize(3);
    for (int i = 0; i < 3; ++i) {
        u(i) = .1 * (i + 1);
        for (int j = 0; j < 3; ++j)
            u(i) += gradient[i][j] * x(j);
    }
}
} // namespace
int main(int argc, char** argv) {
    const std::string device_name = argc > 1 ? argv[1] : "cpu";
    if (device_name == "cuda") {
#ifndef MFEM_USE_CUDA
        return 77;
#else
        if (!std::getenv("CHIPCAE_RUN_GPU"))
            return 77;
#endif
    }
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Init(&argc, &argv);
#endif
    {
        mfem::Device device(device_name.c_str());
        const auto comm = chipcae::Communicator::world();
        auto s = k1::stack();
        auto o = k1::options();
        o.nx = 3;
        o.ny = 3;
        o.ranks = comm.size();
        auto grid = build_tensor_grid(s, o);
        expect(bool(grid), "multilayer grid");
        expect(grid.value().attribute.size() <= 50000, "small CPU case");
        auto adapter = MfemAdapter::create(std::move(grid.value()), comm);
        expect(bool(adapter), "MFEM adapter");
        auto& mesh = adapter.value().local();
        mfem::H1_FECollection fec(1, 3);
#ifdef CHIPCAE_ENABLE_MPI
        mfem::ParFiniteElementSpace fes(
            &adapter.value().parallel(), &fec, 3, mfem::Ordering::byVDIM);
        mfem::ParGridFunction x(&fes), reference(&fes);
        mfem::ParLinearForm b(&fes);
        mfem::ParBilinearForm a(&fes);
#else
        mfem::FiniteElementSpace fes(&mesh, &fec, 3, mfem::Ordering::byVDIM);
        mfem::GridFunction x(&fes), reference(&fes);
        mfem::LinearForm b(&fes);
        mfem::BilinearForm a(&fes);
#endif
        mfem::VectorFunctionCoefficient displacement(3, exact);
        mfem::Vector zero(3);
        zero = 0.0;
        mfem::VectorConstantCoefficient zero_coefficient(zero);
        reference.ProjectCoefficient(displacement);
        mfem::Array<int> boundary(mesh.bdr_attributes.Max());
        boundary = 0;
        for (int i = 0; i < std::min(6, boundary.Size()); ++i)
            boundary[i] = 1;
        mfem::Array<int> essential;
        fes.GetEssentialTrueDofs(boundary, essential);
        x = 0;
        x.ProjectBdrCoefficient(displacement, boundary);
        b = 0;
        mfem::ConstantCoefficient lambda(2), mu(3);
        a.AddDomainIntegrator(new mfem::ElasticityIntegrator(lambda, mu));
        a.Assemble();
        mfem::OperatorPtr A;
        mfem::Vector X, B;
        a.FormLinearSystem(essential, x, b, A, X, B);
#ifdef CHIPCAE_ENABLE_MPI
        mfem::Vector diagonal;
        A.As<mfem::HypreParMatrix>()->GetDiag(diagonal);
        for (int i = 0; i < diagonal.Size(); ++i)
            expect(std::isfinite(diagonal(i)) && diagonal(i) > 0,
                   "eliminated matrix positive finite diagonal");
        if (comm.rank() == 0)
            std::cout << "FormLinearSystem rows="
                      << A.As<mfem::HypreParMatrix>()->GetGlobalNumRows()
                      << " hypre_before_setup=" << HYPRE_GetError() << std::endl;
        // MFEM 4.10 + hypre 3.2.1 sets GENERIC during GetEssentialTrueDofs,
        // before assembly/setup. The trace is saved with the G1 evidence.
        // The eliminated operator is checked above; convergence and independent
        // displacement/strain gates below remain mandatory.
#if HYPRE_RELEASE_NUMBER == 30201
        expect((HYPRE_GetError() & ~HYPRE_ERROR_GENERIC) == 0, "unexpected hypre error bits");
        HYPRE_ClearAllErrors();
#endif
        mfem::HypreBoomerAMG preconditioner(*A.As<mfem::HypreParMatrix>());
#if HYPRE_RELEASE_NUMBER == 30201
        preconditioner.SetErrorMode(mfem::HypreSolver::IGNORE_HYPRE_ERRORS);
#endif
        preconditioner.SetSystemsOptions(3);
        preconditioner.SetPrintLevel(0);
        mfem::CGSolver solver(MPI_COMM_WORLD);
#else
        mfem::GSSmoother preconditioner(*A.As<mfem::SparseMatrix>());
        mfem::CGSolver solver;
#endif
        solver.SetPreconditioner(preconditioner);
        solver.SetOperator(*A);
        solver.SetRelTol(k1::threshold("patch_relative") / 100);
        solver.SetAbsTol(0);
        solver.SetMaxIter(1000);
        solver.SetPrintLevel(0);
        solver.Mult(B, X);
        expect(solver.GetConverged(), "elasticity solve converged");
        a.RecoverFEMSolution(X, b, x);
        double relative =
            x.ComputeL2Error(displacement) / reference.ComputeL2Error(zero_coefficient);
        expect(relative <= k1::threshold("patch_relative"), "linear displacement patch");
        std::vector<double> strain{0, 0};
        for (int e = 0; e < mesh.GetNE(); ++e) {
            auto* t = mesh.GetElementTransformation(e);
            const auto& rule = mfem::IntRules.Get(mfem::Geometry::CUBE, 3);
            for (int q = 0; q < rule.GetNPoints(); ++q) {
                const auto& ip = rule.IntPoint(q);
                t->SetIntPoint(&ip);
                mfem::DenseMatrix g;
                x.GetVectorGradient(*t, g);
                double weight = ip.weight * t->Weight();
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j) {
                        double exact_strain = (gradient[i][j] + gradient[j][i]) / 2;
                        double diff = (g(i, j) + g(j, i)) / 2 - exact_strain;
                        strain[0] += weight * diff * diff;
                        strain[1] += weight * exact_strain * exact_strain;
                    }
            }
        }
        comm.allreduce_sum(strain);
        double strain_relative = std::sqrt(strain[0] / strain[1]);
        expect(strain_relative <= k1::threshold("patch_relative"), "constant strain patch");
        if (comm.rank() == 0)
            std::cout << "patch displacement=" << relative << " strain=" << strain_relative << '\n';
    }
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Finalize();
#endif
}
