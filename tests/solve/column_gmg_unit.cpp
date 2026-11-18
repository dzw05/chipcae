#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"
#include "chipcae/solvers/fem/column_gmg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <numeric>
#include <petscksp.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace chipcae;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

int size_as_int(std::size_t size) {
    require(size <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
            "fixture size overflow");
    return static_cast<int>(size);
}

mesh::HexMeshArrays regular_grid(int levels) {
    mesh::StackModel stack;
    stack.outline.box = {0, 0, 1, 1};
    mesh::Layer layer;
    layer.name = layer.material = "bottom";
    layer.kind = "Explicit";
    layer.thickness = .25;
    layer.n_elem_min = 2;
    stack.layers.push_back(layer);
    layer.name = layer.material = "top";
    stack.layers.push_back(layer);
    mesh::GridOptions options;
    options.nx = options.ny = 8;
    options.levels = levels;
    options.max_ratio = 100;
    auto result = mesh::build_tensor_grid(stack, options);
    require(static_cast<bool>(result), "two-layer unit-box grid construction");
    auto arrays = std::move(result.value());
    require(arrays.grid_report.levels == levels, "fixture levels must match the requested levels");
    require(arrays.grid_report.level_axes.size() == static_cast<std::size_t>(levels),
            "fixture must report all levels");
    require(arrays.grid_report.padding == std::array<std::size_t, 2>{0, 0},
            "unit-box fixture must have zero padding");
    require(arrays.axes[2].size() >= 5, "two layers must supply at least four z cells");
    for (int k = 0; k < levels; ++k) {
        // Literal finest size 8: levels=2 gives 4->8; levels=3 gives 2->4->8.
        const int cells = 8 / (1 << (levels - 1 - k));
        for (int axis = 0; axis < 2; ++axis)
            require(arrays.grid_report.level_axes[k][axis].size() ==
                        static_cast<std::size_t>(cells + 1),
                    "fixture must have the prescribed cell counts, without adapting expectations");
    }
    return arrays;
}

mfem::Mesh serial_mesh(mesh::HexMeshArrays& arrays) {
    mfem::Mesh grid(arrays.vertices.data(),
                    size_as_int(arrays.node_gid.size()),
                    arrays.hex.data(),
                    mfem::Geometry::CUBE,
                    arrays.attribute.data(),
                    size_as_int(arrays.attribute.size()),
                    arrays.bdr_quad.data(),
                    mfem::Geometry::SQUARE,
                    arrays.bdr_attribute.data(),
                    size_as_int(arrays.bdr_attribute.size()),
                    3,
                    3);
    grid.Finalize(false, false);
    return grid;
}

struct Fixture {
    mesh::HexMeshArrays arrays;
    mfem::Mesh grid;
    mfem::H1_FECollection collection{2, 3};
    mfem::FiniteElementSpace space;
    mfem::FiniteElementSpace coordinate_space;
    mfem::GridFunction coordinates;
    mfem::Array<int> essential;

    Fixture(int levels, int components, mfem::Ordering::Type ordering = mfem::Ordering::byVDIM)
        : arrays(regular_grid(levels)), grid(serial_mesh(arrays)),
          space(&grid, &collection, components, ordering),
          coordinate_space(&grid, &collection, 3, mfem::Ordering::byNODES),
          coordinates(&coordinate_space) {
        mfem::VectorFunctionCoefficient identity(
            3, [](const mfem::Vector& p, mfem::Vector& v) { v = p; });
        coordinates.ProjectCoefficient(identity);
        int corners = 0;
        for (int s = 0; s < space.GetNDofs(); ++s) {
            const auto p = point(s);
            if (std::abs(p[2]) < 1e-13)
                for (int c = 0; c < components; ++c)
                    essential.Append(space.DofToVDof(s, c));
            // zmax makes this extra x constraint independent of the constrained zmin face.
            if (components == 3 && std::abs(p[0]) < 1e-13 && std::abs(p[1]) < 1e-13 &&
                std::abs(p[2] - .5) < 1e-13) {
                essential.Append(space.DofToVDof(s, 0));
                ++corners;
            }
        }
        require(essential.Size() == 17 * 17 * components + (components == 3 ? 1 : 0),
                "fixture must constrain every zmin component and only the extra zmax x corner");
        require(corners == (components == 3 ? 1 : 0),
                "extra vector constraint must be nonredundant");
    }

    std::array<double, 3> point(int scalar_dof) const {
        return {coordinates[coordinate_space.DofToVDof(scalar_dof, 0)],
                coordinates[coordinate_space.DofToVDof(scalar_dof, 1)],
                coordinates[coordinate_space.DofToVDof(scalar_dof, 2)]};
    }
};

std::vector<double> q2_axis(const std::vector<double>& cells) {
    std::vector<double> nodes;
    for (std::size_t i = 1; i < cells.size(); ++i) {
        nodes.push_back(cells[i - 1]);
        nodes.push_back(.5 * (cells[i - 1] + cells[i]));
    }
    nodes.push_back(cells.back());
    return nodes;
}

// Independent bit interleaving checks the public Morton ordering, rather than using its builder.
std::uint64_t morton(int x, int y) {
    std::uint64_t key = 0;
    for (unsigned bit = 0; bit < 31; ++bit) {
        key |= static_cast<std::uint64_t>((x >> bit) & 1) << (2 * bit);
        key |= static_cast<std::uint64_t>((y >> bit) & 1) << (2 * bit + 1);
    }
    return key;
}

std::vector<std::pair<int, int>> columns(int nodes_x, int nodes_y) {
    std::vector<std::pair<int, int>> result;
    for (int y = 0; y < nodes_y; ++y)
        for (int x = 0; x < nodes_x; ++x)
            result.emplace_back(x, y);
    std::sort(result.begin(), result.end(), [](auto a, auto b) {
        return morton(a.first, a.second) < morton(b.first, b.second);
    });
    return result;
}

double polynomial(double x, double y, double z) {
    return 1 + x + y * y + x * y + z * z;
}

void check_prolongation(int levels, int components, mfem::Ordering::Type ordering) {
    Fixture f(levels, components, ordering);
    const auto h = fem::build_column_gmg_hierarchy(f.arrays, false, 1, f.space, f.essential);
    require(h.L == levels - 1 && h.ncomp == components, "supported hierarchy metadata");
    require(h.levels.size() == static_cast<std::size_t>(levels) &&
                h.P.size() == static_cast<std::size_t>(levels - 1),
            "hierarchy must contain L+1 levels and L interpolations");
    const auto z = q2_axis(f.arrays.axes[2]);
    require(h.nz_nodes == size_as_int(z.size()), "z lattice must remain unchanged on every level");
    const auto slots = static_cast<std::int64_t>(z.size()) * components;
    std::vector<int> component_of(f.space.GetTrueVSize());
    std::vector<std::array<double, 3>> point_of(f.space.GetTrueVSize());
    std::vector<std::uint8_t> essential_tdof(f.space.GetTrueVSize(), 0);
    for (int tdof : f.essential)
        essential_tdof[tdof] = 1;
    for (int s = 0; s < f.space.GetNDofs(); ++s)
        for (int c = 0; c < components; ++c) {
            const auto tdof = f.space.DofToVDof(s, c);
            point_of[tdof] = f.point(s);
            component_of[tdof] = c;
        }
    std::vector<mfem::Vector> values;
    std::vector<std::int64_t> expected_rows;
    for (int k = 0; k <= h.L; ++k) {
        const auto& level = h.levels[k];
        const auto x = q2_axis(f.arrays.grid_report.level_axes[k][0]);
        const auto y = q2_axis(f.arrays.grid_report.level_axes[k][1]);
        require(level.x.size() == x.size() && level.y.size() == y.size(), "Q2 axis dimensions");
        for (std::size_t i = 0; i < x.size(); ++i)
            require(std::abs(level.x[i] - x[i]) < 1e-14, "physical Q2 x coordinates");
        for (std::size_t i = 0; i < y.size(); ++i)
            require(std::abs(level.y[i] - y[i]) < 1e-14, "physical Q2 y coordinates");
        const auto xy = columns(size_as_int(x.size()), size_as_int(y.size()));
        const auto full_rows = static_cast<std::int64_t>(xy.size()) * slots;
        const auto rows =
            full_rows - (k == h.L ? 0 : static_cast<std::int64_t>(xy.size()) * components);
        expected_rows.push_back(rows);
        require(level.col_off.size() == xy.size() + 1, "every in-plane Q2 node must have a block");
        require(level.col_dof.size() == static_cast<std::size_t>(rows), "operator row count");
        require(level.essential.size() == static_cast<std::size_t>(full_rows),
                "full-layout essential mask");
        require(level.col_off.front() == 0 && level.col_off.back() == rows,
                "block offsets span the operator");
        mfem::Vector q(size_as_int(level.col_dof.size()));
        std::int64_t row = 0;
        for (std::size_t c = 0; c < xy.size(); ++c) {
            require(level.col_off[c] == row, "contiguous Morton column blocks");
            const auto [ix, iy] = xy[c];
            for (std::size_t iz = 0; iz < z.size(); ++iz)
                for (int comp = 0; comp < components; ++comp) {
                    const auto full = static_cast<std::int64_t>(c) * slots +
                                      static_cast<std::int64_t>(iz) * components + comp;
                    if (k < h.L) {
                        require(level.essential[full] == (iz == 0 ? 1 : 0),
                                "E_k must contain exactly zmin components, with no isolated-corner "
                                "DOF");
                        if (iz == 0)
                            continue;
                        require(level.col_dof[row] == full,
                                "coarse compaction must preserve full ordering");
                    } else {
                        const auto tdof = level.col_dof[row];
                        require(tdof >= 0 && tdof < f.space.GetTrueVSize(), "finest tdof range");
                        const auto p = point_of[tdof];
                        require(
                            component_of[tdof] == comp && std::abs(p[0] - x[ix]) < 1e-13 &&
                                std::abs(p[1] - y[iy]) < 1e-13 && std::abs(p[2] - z[iz]) < 1e-13,
                            "finest rows must follow Morton columns, ascending z, then component");
                        require(level.essential[full] == essential_tdof[tdof],
                                "finest essential tdof mask");
                    }
                    q[static_cast<int>(row++)] = polynomial(x[ix], y[iy], z[iz]);
                }
            require(level.col_off[c + 1] == row, "column ends after its last retained component");
        }
        require(row == rows, "compact row count");
        values.push_back(std::move(q));
    }
    auto permutation = h.levels.back().col_dof;
    std::sort(permutation.begin(), permutation.end());
    require(permutation.size() == static_cast<std::size_t>(f.space.GetTrueVSize()),
            "finest full layout size");
    for (std::size_t i = 0; i < permutation.size(); ++i)
        require(permutation[i] == static_cast<std::int64_t>(i),
                "finest col_dof must be a tdof permutation");
    for (int k = 0; k < h.L; ++k) {
        const auto& p = h.P[k];
        require(p.Height() == expected_rows[k + 1] && p.Width() == expected_rows[k],
                "rectangular interpolation dimensions");
        require(p.Finalized(), "interpolation must expose finalized CSR storage");
        std::vector<int> column_nnz(p.Width(), 0);
        mfem::Vector actual(p.Height());
        p.Mult(values[k], actual);
        const double tolerance = 1e-12 * values[k + 1].Normlinf();
        for (int r = 0; r < p.Height(); ++r) {
            const auto fine_full = k + 1 == h.L ? r : h.levels[k + 1].col_dof[r];
            const bool constrained = h.levels[k + 1].essential[fine_full] != 0;
            const int first = p.GetI()[r], last = p.GetI()[r + 1];
            require(last - first <= 9,
                    "one component row must have at most nine interpolation entries");
            require(!constrained || first == last,
                    "essential interpolation rows must be structurally zero");
            for (int j = first; j < last; ++j) {
                const int col = p.GetJ()[j];
                require(col >= 0 && col < p.Width(), "interpolation column range");
                require(std::isfinite(p.GetData()[j]) && std::abs(p.GetData()[j]) > 1e-14,
                        "interpolation must omit zero weights");
                ++column_nnz[col];
                require(h.levels[k].col_dof[col] % slots == fine_full % slots,
                        "interpolation must preserve z and component independently");
            }
            const double expected = constrained ? 0 : values[k + 1][r];
            require(std::abs(actual[r] - expected) <= tolerance,
                    "P must reproduce 1+x+y^2+xy+z^2 on free rows and zero constrained rows");
        }
        require(std::all_of(column_nnz.begin(), column_nnz.end(), [](int n) { return n > 0; }),
                "every compact coarse column must contain a nonzero");
    }
}

void expect_unsupported(const mesh::HexMeshArrays& arrays,
                        bool has_cut,
                        int np,
                        const Fixture& f,
                        const std::string& diagnostic) {
    try {
        (void)fem::build_column_gmg_hierarchy(arrays, has_cut, np, f.space, f.essential);
    } catch (const fem::ColumnGmgUnsupported& e) {
        require(std::string(e.what()).find("column_gmg unsupported: " + diagnostic) !=
                    std::string::npos,
                "unsupported diagnostic: expected '" + diagnostic + "', got '" + e.what() + "'");
        return;
    } catch (const std::exception& e) {
        throw std::runtime_error("expected ColumnGmgUnsupported for " + diagnostic +
                                 ", got: " + e.what());
    }
    throw std::runtime_error("expected ColumnGmgUnsupported for " + diagnostic);
}

struct SpdSystem {
    Fixture fixture{2, 1};
    mfem::FunctionCoefficient conductivity{
        [](const mfem::Vector& p) { return 1 + 20 * p[0] * p[0] + 7 * p[1] * p[2]; }};
    mfem::ConstantCoefficient mass{1};
    mfem::BilinearForm form{&fixture.space};
    mfem::SparseMatrix A;
    mfem::Vector b, x;

    SpdSystem() {
        form.SetDiagonalPolicy(mfem::Operator::DIAG_ONE);
        form.AddDomainIntegrator(new mfem::DiffusionIntegrator(conductivity));
        form.AddDomainIntegrator(new mfem::MassIntegrator(mass));
        form.Assemble();
        form.Finalize();
        mfem::GridFunction initial(&fixture.space);
        initial = 0.;
        mfem::Vector load(fixture.space.GetVSize());
        load = 0.;
        mfem::Vector formed_x, formed_b;
        form.FormLinearSystem(fixture.essential, initial, load, A, formed_x, formed_b);
        // FormLinearSystem may alias its input vectors. Keep the solve buffers owned after
        // the temporary GridFunction and load leave this constructor's scope.
        x.SetSize(formed_x.Size());
        x = 0.;
        b.SetSize(formed_b.Size());
        b = formed_b;
        mfem::Vector exact(A.Width());
        // Several frequencies prevent a one-step CG solve on this deterministic SPD operator.
        for (int i = 0; i < exact.Size(); ++i)
            exact[i] = std::sin(.37 * (i + 1)) + .4 * std::cos(.13 * (i + 1));
        for (int tdof : fixture.essential)
            exact[tdof] = 0.;
        A.Mult(exact, b);
        require(b.Norml2() > 0, "SPD fixture must have a nonzero load");
        // Exercise the distinction between KSP's ||b|| denominator and the reported max(||b||,1).
        b *= .125 / b.Norml2();
    }

    fem::ColumnGmgStats solve(const mfem::Array<int>& essential,
                              const fem::ColumnGmgOptions& options = {}) {
        return fem::solve_column_gmg(
            fixture.arrays, false, 1, fixture.space, A, essential, b, x, options);
    }

    double residual() const {
        mfem::Vector r(b.Size());
        A.Mult(x, r);
        r -= b;
        return r.Norml2();
    }
};

void check_zero_rhs() {
    SpdSystem s;
    s.b = 0.;
    s.x = 7.;
    const auto stats = s.solve(s.fixture.essential);
    require(stats.iterations == 0, "zero RHS must take exactly zero iterations");
    require(stats.relative_residual == 0. && s.x.Normlinf() == 0.,
            "zero RHS must return exact zero residual and solution");
}

void check_solve_and_duplicates() {
    SpdSystem s;
    fem::ColumnGmgOptions options;
    options.rtol = 1e-10;
    const auto stats = s.solve(s.fixture.essential, options);
    const double residual = s.residual();
    require(stats.iterations > 1 &&
                stats.iterations <= static_cast<std::uint32_t>(options.max_iter),
            "normal SPD solve must require multiple iterations and converge within max_iter");
    require(residual <= options.rtol * s.b.Norml2(),
            "actual unpreconditioned residual must satisfy rtol*||b||");
    require(std::abs(stats.relative_residual - residual / std::max(s.b.Norml2(), 1.)) <= 1e-14,
            "reported residual must use max(||b||,1), independently of KSP stopping");
    require(std::isfinite(stats.setup_seconds) && stats.setup_seconds >= 0 &&
                std::isfinite(stats.solve_seconds) && stats.solve_seconds >= 0,
            "finite nonnegative timings");
    const auto nz = static_cast<std::int64_t>(s.fixture.arrays.axes[2].size() - 1);
    require(stats.level_rows == std::vector<std::int64_t>{9 * 9 * (2 * nz), 17 * 17 * (2 * nz + 1)},
            "solver statistics must expose compact coarse rows and full finest rows");
    require(stats.level_blocks == std::vector<std::int64_t>{17 * 17},
            "one block for each smoothed Q2 column");
    const mfem::Vector reference(s.x);
    mfem::Array<int> duplicated(s.fixture.essential);
    for (int tdof : s.fixture.essential)
        duplicated.Append(tdof);
    s.x = 11.;
    const auto again = s.solve(duplicated, options);
    require(s.residual() <= options.rtol * s.b.Norml2(),
            "duplicate essential indices must preserve convergence");
    s.x -= reference;
    require(s.x.Normlinf() <= 1e-10 * reference.Normlinf(),
            "duplicate essential indices must preserve the solution");
    require(again.level_rows == stats.level_rows && again.level_blocks == stats.level_blocks,
            "duplicate essential indices must preserve hierarchy dimensions");
}

void check_divergence() {
    SpdSystem s;
    fem::ColumnGmgOptions options;
    options.max_iter = 1;
    try {
        (void)s.solve(s.fixture.essential, options);
    } catch (const fem::ColumnGmgUnsupported& e) {
        throw std::runtime_error(
            std::string("divergence must never become unsupported/fallback: ") + e.what());
    } catch (const std::runtime_error& e) {
        const std::string message = e.what();
        require(message.find("column_gmg diverged: reason=") != std::string::npos &&
                    message.find("iterations=1") != std::string::npos &&
                    message.find("relative_residual=") != std::string::npos,
                "max_iter=1 must report KSP divergence reason, iterations and residual; got: " +
                    message);
        return;
    }
    throw std::runtime_error("max_iter=1 must throw an ordinary solver runtime_error");
}

void check_setup_error(bool singular_block) {
    Fixture f(2, 1);
    mfem::Vector diagonal(f.space.GetTrueVSize());
    diagonal = singular_block ? 1. : -1.;
    for (int tdof : f.essential)
        diagonal[tdof] = 1.;
    if (singular_block) {
        int zero_diagonals = 0;
        for (int s = 0; s < f.space.GetNDofs(); ++s) {
            const auto p = f.point(s);
            if (std::abs(p[0]) < 1e-13 && std::abs(p[1]) < 1e-13 && std::abs(p[2] - .5) < 1e-13) {
                diagonal[f.space.DofToVDof(s, 0)] = 0.;
                ++zero_diagonals;
            }
        }
        require(zero_diagonals == 1, "singular-block fixture must zero one free diagonal");
    }
    mfem::SparseMatrix a(diagonal);
    mfem::Vector b(diagonal.Size()), x;
    b = 1.;
    const std::string expected = singular_block
                                     ? "column_gmg: singular column block level 1 block 0"
                                     : "column_gmg: coarse operator level 0 not positive";
    try {
        (void)fem::solve_column_gmg(f.arrays, false, 1, f.space, a, f.essential, b, x);
    } catch (const fem::ColumnGmgUnsupported& e) {
        throw std::runtime_error(std::string("setup error must not trigger fallback: ") + e.what());
    } catch (const std::runtime_error& e) {
        require(std::string(e.what()) == expected,
                "callback diagnostic must reach C++ boundary: " + std::string(e.what()));
        return;
    }
    throw std::runtime_error("expected setup error: " + expected);
}

} // namespace

int main(int argc, char** argv) {
    if (PetscInitialize(&argc, &argv, nullptr, nullptr))
        return 2;
    int failures = 0;
    const auto run = [&](const std::string& name, const std::function<void()>& test) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    const std::string mode = argc == 2 ? argv[1] : "";
    if (mode == "prolongation") {
        for (int levels : {2, 3}) {
            run("scalar levels=" + std::to_string(levels),
                [=] { check_prolongation(levels, 1, mfem::Ordering::byVDIM); });
            for (auto ordering : {mfem::Ordering::byVDIM, mfem::Ordering::byNODES})
                run("vector ordering=" + std::to_string(ordering) +
                        " levels=" + std::to_string(levels),
                    [=] { check_prolongation(levels, 3, ordering); });
        }
    } else if (mode == "unsupported") {
        run("real const-overload cut", [] {
            Fixture f(2, 1);
            mesh::cut::CutGeometry geometry;
            for (const auto& [name, body] : f.arrays.attr_table.bodies)
                geometry.planes.push_back({body, {1, 0, 0}, .3});
            const auto cut = mesh::cut::build_cut_layer(
                std::as_const(f.arrays), geometry, mesh::cut::CutOptions{});
            require(static_cast<bool>(cut) && !cut.value().cut_elem.empty(),
                    "cut fixture must contain actual cut elements");
            expect_unsupported(f.arrays, static_cast<bool>(cut), 1, f, "cut mesh");
        });
        run("pending cut", [] {
            Fixture f(2, 1);
            f.arrays.pending_cut.push_back(0);
            expect_unsupported(f.arrays, false, 1, f, "cut mesh");
        });
        run("np=2", [] {
            Fixture f(2, 1);
            expect_unsupported(f.arrays, false, 2, f, "np=2");
        });
        run("single level", [] {
            Fixture f(1, 1);
            expect_unsupported(f.arrays, false, 1, f, "single level");
        });
        run("synchronized deleted cell", [] {
            Fixture f(2, 1);
            f.arrays.hex.erase(f.arrays.hex.begin(), f.arrays.hex.begin() + 8);
            f.arrays.attribute.erase(f.arrays.attribute.begin());
            f.arrays.elem_gid.erase(f.arrays.elem_gid.begin());
            f.arrays.partitioning.erase(f.arrays.partitioning.begin());
            expect_unsupported(f.arrays, false, 1, f, "non-regular column -1");
        });
        run("R3 duplicate GID", [] {
            Fixture f(2, 1);
            f.arrays.elem_gid[1] = f.arrays.elem_gid[0];
            expect_unsupported(f.arrays, false, 1, f, "non-regular column 0");
        });
        run("R4 GID/geometry mismatch", [] {
            Fixture f(2, 1);
            // Preserve unique/in-range GIDs and cell counts; only their z geometry is wrong.
            // Both offending cells are in dense column 0, independent of traversal order.
            std::swap(f.arrays.elem_gid[0], f.arrays.elem_gid[1]);
            expect_unsupported(f.arrays, false, 1, f, "non-regular column 0");
        });
        run("zero RHS", check_zero_rhs);
        run("SPD solve and duplicate essential indices", check_solve_and_duplicates);
        run("max_iter divergence is not unsupported", check_divergence);
        run("coarse nonpositive diagonal", [] { check_setup_error(false); });
        run("singular column callback diagnostic", [] { check_setup_error(true); });
    } else {
        std::cerr << "expected prolongation or unsupported\n";
        ++failures;
    }
    if (PetscFinalize())
        return 2;
    return failures == 0 ? 0 : 1;
}
