#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"
#include "chipcae/solvers/fem/cut_fem.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
using namespace chipcae;
using namespace mfem;
int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    try {
        if (argc != 2)
            throw std::runtime_error("expected output matrix file");
        std::ofstream out(argv[1]);
        out << std::setprecision(17);
        for (bool layered : {false, true})
            for (double aspect : {1., .1})
                for (bool elastic : {false, true})
                    for (int position = 0; position < 50; ++position) {
                        mesh::StackModel s;
                        s.outline.box = {0, 0, 1, 1};
                        mesh::Layer l;
                        l.name = "solid";
                        l.material = "solid";
                        l.kind = "Explicit";
                        l.thickness = aspect;
                        l.n_elem_min = 2;
                        s.layers = {l};
                        if (layered) {
                            s.layers[0].thickness = aspect / 2;
                            s.layers[0].n_elem_min = 1;
                            l.name = l.material = "second";
                            l.thickness = aspect / 2;
                            l.n_elem_min = 1;
                            s.layers.push_back(l);
                        }
                        mesh::GridOptions o;
                        o.nx = o.ny = 2;
                        o.levels = 1;
                        o.max_ratio = 100;
                        auto arrays = mesh::build_tensor_grid(s, o).value();
                        double location =
                            .5 + .5 * (position == 0 ? 1e-8 : static_cast<double>(position) / 50);
                        mesh::cut::CutOptions co;
                        mesh::cut::CutGeometry geometry;
                        for (auto [name, body] : arrays.attr_table.bodies)
                            geometry.planes.push_back({body, {1, 0, 0}, location});
                        auto cut = mesh::cut::build_cut_layer(arrays, geometry, co).value();
                        if (position == 0 && *std::min_element(cut.sub_volfrac.begin(),
                                                               cut.sub_volfrac.end()) >= 1e-6)
                            throw std::runtime_error(
                                "scan must contain a sub-volume fraction below 1e-6");
                        Mesh grid(arrays.vertices.data(),
                                  arrays.node_gid.size(),
                                  arrays.hex.data(),
                                  Geometry::CUBE,
                                  arrays.attribute.data(),
                                  arrays.attribute.size(),
                                  arrays.bdr_quad.data(),
                                  Geometry::SQUARE,
                                  arrays.bdr_attribute.data(),
                                  arrays.bdr_attribute.size(),
                                  3,
                                  3);
                        grid.Finalize(false, false);
                        if (layered && position == 0) {
                            auto interface = cut;
                            interface.gp_face_gid.clear();
                            interface.gp_elem_gid = {};
                            for (std::size_t f = 0; f < cut.gp_face_gid.size(); ++f) {
                                auto a = std::find(arrays.elem_gid.begin(),
                                                   arrays.elem_gid.end(),
                                                   cut.gp_elem_gid[0][f]) -
                                         arrays.elem_gid.begin();
                                auto b = std::find(arrays.elem_gid.begin(),
                                                   arrays.elem_gid.end(),
                                                   cut.gp_elem_gid[1][f]) -
                                         arrays.elem_gid.begin();
                                if (arrays.attribute[a] != arrays.attribute[b]) {
                                    interface.gp_face_gid.push_back(cut.gp_face_gid[f]);
                                    for (int side = 0; side < 2; ++side)
                                        interface.gp_elem_gid[side].push_back(
                                            cut.gp_elem_gid[side][f]);
                                }
                            }
                            H1_FECollection collection(2, 3);
                            FiniteElementSpace space(
                                &grid, &collection, elastic ? 3 : 1, Ordering::byVDIM);
                            BilinearForm gp(&space);
                            gp.AddInteriorFaceIntegrator(new fem::GhostPenalty(
                                arrays, interface, std::vector<double>{1, 2}, .1, elastic ? 3 : 1));
                            gp.Assemble();
                            gp.Finalize(0);
                            if (interface.gp_face_gid.empty() || gp.SpMat().MaxNorm() == 0)
                                throw std::runtime_error(
                                    "material-interface ghost faces must be stabilized");
                        }
                        fem::CutRules rules(cut, grid);
                        H1_FECollection collection(2, 3);
                        FiniteElementSpace space(
                            &grid, &collection, elastic ? 3 : 1, Ordering::byVDIM);
                        for (double gamma : {0., .1}) {
                            Vector conductivity(layered ? 2 : 1);
                            conductivity[0] = 1;
                            if (layered)
                                conductivity[1] = 2;
                            PWConstCoefficient k(conductivity);
                            std::vector<double> scale{elastic ? 3. : 1.};
                            if (layered)
                                scale.push_back(2 * scale[0]);
                            BilinearForm form(&space);
                            if (elastic)
                                form.AddDomainIntegrator(
                                    new fem::CutBilinear(rules, new ElasticityIntegrator(k, k)));
                            else
                                form.AddDomainIntegrator(
                                    new fem::CutBilinear(rules, new DiffusionIntegrator(k)));
                            form.AddInteriorFaceIntegrator(
                                new fem::GhostPenalty(arrays, cut, scale, gamma, elastic ? 3 : 1));
                            form.Assemble();
                            form.Finalize(0);
                            Array<int> marker(grid.bdr_attributes.Max());
                            marker = 0;
                            marker[0] = 1;
                            Array<int> essential;
                            space.GetEssentialTrueDofs(marker, essential);
                            GridFunction u(&space);
                            u = 0.;
                            LinearForm rhs(&space);
                            rhs = 0.;
                            OperatorPtr op;
                            Vector X, B;
                            form.FormLinearSystem(essential, u, rhs, op, X, B);
                            auto& a = *op.As<SparseMatrix>();
                            DenseMatrix dense;
                            a.ToDenseMatrix(dense);
                            out << layered << ' ' << aspect << ' ' << elastic << ' ' << position
                                << ' ' << gamma << ' ' << a.Height() << '\n';
                            for (int i = 0; i < a.Height(); ++i) {
                                for (int j = 0; j < a.Height(); ++j)
                                    out << dense(i, j) << ' ';
                                out << '\n';
                            }
                            // Consistency: derivative jumps of a global Q2 polynomial vanish.
                            BilinearForm gp(&space);
                            gp.AddInteriorFaceIntegrator(
                                new fem::GhostPenalty(arrays, cut, scale, .1, elastic ? 3 : 1));
                            gp.Assemble();
                            gp.Finalize(0);
                            FunctionCoefficient polynomial([layered](const Vector& x) {
                                return x[0] * x[0] +
                                       (layered ? x[1] * x[1] : x[1] * x[2] + x[2] * x[2]);
                            });
                            if (elastic) {
                                VectorFunctionCoefficient vector_polynomial(
                                    3, [layered](const Vector& x, Vector& value) {
                                        value.SetSize(3);
                                        for (int c = 0; c < 3; ++c)
                                            value[c] =
                                                (c + 1) * (x[0] * x[0] +
                                                           (layered ? x[1] * x[1]
                                                                    : x[1] * x[2] + x[2] * x[2]));
                                    });
                                u.ProjectCoefficient(vector_polynomial);
                            } else
                                u.ProjectCoefficient(polynomial);
                            Vector residual(space.GetVSize());
                            gp.Mult(u, residual);
                            if (residual.Norml2() > 1e-10)
                                throw std::runtime_error("ghost penalty Q2 patch consistency");
                        }
                    }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    MPI_Finalize();
}
