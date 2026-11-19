#include "chipcae/solvers/fvm/thermal.hpp"

#include "mfd_local.hpp"
#include "petsc_util.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
namespace chipcae::solvers::fvm {
namespace {
using detail::Dense;
mesh::Error invalid(const std::string& reason) {
    return {"fvm/thermal", "INPUT_INVALID: " + reason};
}
struct Boundary {
    int kind = -1;
    long double value = 0, h = 0;
};
std::size_t root(std::vector<std::size_t>& parent, std::size_t a) {
    while (parent[a] != a) {
        parent[a] = parent[parent[a]];
        a = parent[a];
    }
    return a;
}
} // namespace
Expected<ThermalResult> solve_thermal(const FvGrid& g,
                                      const mesh::HexMeshArrays& m,
                                      const ThermalProblem& p,
                                      MPI_Comm comm) {
    const auto nc = g.cv_volume.size(), nf = g.faces.size();
    int rank = 0, ranks = 0;
    MPI_Comm_rank(comm, &rank);
    MPI_Comm_size(comm, &ranks);
    if (p.k.size() != nc || p.q_vol.size() != nc || p.fixed_cv.size() != p.fixed_T.size() || !nc ||
        g.cv_rank.size() != nc || g.cv_elem.size() != nc || g.cv_layer.size() != nc ||
        g.cv_elem_gid.size() != nc || g.cv_attribute.size() != nc || g.cv_centroid.size() != nc ||
        g.cv_face_off.size() != nc + 1)
        return invalid("inconsistent problem/grid array lengths");
    if (g.cv_face_off.front() != 0 || g.cv_face_off.back() < 0 ||
        static_cast<std::size_t>(g.cv_face_off.back()) != g.cv_faces.size())
        return invalid("invalid CSR endpoints");
    for (const auto& axis : g.axes) {
        if (axis.size() < 2)
            return invalid("invalid grid axes");
        for (std::size_t i = 0; i < axis.size(); ++i)
            if (!std::isfinite(axis[i]) || (i && !(axis[i] > axis[i - 1])))
                return invalid("invalid grid axes");
    }
    if ((p.preconditioner != "jacobi" && p.preconditioner != "gamg") ||
        (p.stabilization != "diag" && p.stabilization != "scalar") || !(p.rtol > 0 && p.rtol < 1) ||
        !std::isfinite(p.rtol) || p.max_iter <= 0 || (p.T_ref && !std::isfinite(*p.T_ref)))
        return invalid("invalid solver options");
    for (std::size_t c = 0; c < nc; ++c) {
        if (!std::isfinite(p.q_vol[c]) || g.cv_rank[c] < 0 || g.cv_rank[c] >= ranks)
            return invalid("invalid source or CV rank");
        for (auto k : p.k[c])
            if (!std::isfinite(k) || !(k > 0))
                return invalid("invalid conductivity gid=" + std::to_string(g.cv_elem_gid[c]));
    }
    std::vector<std::optional<long double>> fixed(nc);
    for (std::size_t i = 0; i < p.fixed_cv.size(); ++i) {
        auto c = p.fixed_cv[i];
        if (c < 0 || static_cast<std::size_t>(c) >= nc || !std::isfinite(p.fixed_T[i]))
            return invalid("invalid fixed CV");
        if (fixed[c] && *fixed[c] != p.fixed_T[i])
            return invalid("conflicting fixed CV");
        fixed[c] = p.fixed_T[i];
    }
    std::vector<Boundary> bc(nf);
    const std::array<std::string, 6> names{
        "face:xmin", "face:xmax", "face:ymin", "face:ymax", "face:zmin", "face:zmax"};
    long double ref_sum = 0, ref_area = 0;
    for (const auto& b : p.bcs) {
        if (!std::isfinite(b.T) || !std::isfinite(b.h) || !std::isfinite(b.T_inf) ||
            !std::isfinite(b.q) || b.h < 0)
            return invalid("invalid boundary coefficient");
        if (b.face && *b.face >= nf)
            return invalid("invalid face selector");
        if (b.target == "boundary:embedded" && b.kind != ThermalBc::Kind::Robin)
            return invalid("embedded boundary supports Robin or insulation");
        std::vector<std::size_t> selected;
        for (std::size_t f = 0; f < nf; ++f) {
            const auto& face = g.faces[f];
            if (face.lo < 0 || static_cast<std::size_t>(face.lo) >= nc ||
                face.hi >= static_cast<std::int64_t>(nc))
                return invalid("invalid face incidence");
            if (face.hi >= 0 || (b.face && *b.face != f))
                continue;
            bool match = b.target == "boundary:embedded" && face.side == 6;
            if (face.side >= 0 && face.side < 6 && face.axis >= 0) {
                const auto& axis = g.axes[face.axis];
                const auto coordinate = face.side % 2 ? axis.back() : axis.front();
                match |= b.target == names[face.side] &&
                         (b.face || std::abs(face.centroid[face.axis] - coordinate) <=
                                        1e-12 * (axis.back() - axis.front()));
            }
            if (b.target.starts_with("layer:")) {
                auto it = m.attr_table.layers.find(b.target.substr(6));
                match = it != m.attr_table.layers.end() && g.cv_layer[face.lo] == it->second;
            }
            if (match)
                selected.push_back(f);
        }
        if (selected.empty())
            return invalid("target has no boundary faces: " + b.target);
        for (auto f : selected) {
            if (bc[f].kind >= 0)
                return invalid("duplicate boundary prescription");
            if (g.faces[f].side == 6 && b.kind != ThermalBc::Kind::Robin)
                return invalid("embedded boundary supports Robin or insulation");
            auto& v = bc[f];
            v.kind = static_cast<int>(b.kind);
            v.h = b.h;
            switch (b.kind) {
            case ThermalBc::Kind::Dirichlet:
                v.value = b.T;
                break;
            case ThermalBc::Kind::Robin:
                v.value = b.T_inf;
                break;
            case ThermalBc::Kind::Flux:
                v.value = b.q;
                break;
            default:
                return invalid("unknown boundary kind");
            }
            if (v.kind == 0 || (v.kind == 1 && v.h > 0)) {
                ref_sum += g.faces[f].area * v.value;
                ref_area += g.faces[f].area;
            }
        }
    }
    long double reference = p.T_ref ? *p.T_ref : (ref_area > 0 ? ref_sum / ref_area : 0);
    // A fixed-only problem uses the volume-weighted fixed temperature as origin.
    if (!p.T_ref && !(ref_area > 0)) {
        long double w = 0;
        for (std::size_t c = 0; c < nc; ++c)
            if (fixed[c]) {
                reference += *fixed[c] * g.cv_volume[c];
                w += g.cv_volume[c];
            }
        if (w > 0)
            reference /= w;
    }
    std::vector<std::size_t> parent(nc);
    std::iota(parent.begin(), parent.end(), 0);
    for (const auto& f : g.faces) {
        if (f.lo < 0 || static_cast<std::size_t>(f.lo) >= nc ||
            f.hi >= static_cast<std::int64_t>(nc))
            return invalid("invalid face incidence");
        if (f.hi >= 0)
            parent[root(parent, static_cast<std::size_t>(f.hi))] =
                root(parent, static_cast<std::size_t>(f.lo));
    }
    std::vector<bool> anchored(nc);
    for (std::size_t c = 0; c < nc; ++c)
        if (fixed[c])
            anchored[root(parent, c)] = true;
    for (std::size_t f = 0; f < nf; ++f)
        if (bc[f].kind == 0 || (bc[f].kind == 1 && bc[f].h > 0))
            anchored[root(parent, static_cast<std::size_t>(g.faces[f].lo))] = true;
    for (std::size_t c = 0; c < nc; ++c)
        if (!anchored[root(parent, c)])
            return invalid("unanchored connected component (pure Neumann)");
    for (auto& f : fixed)
        if (f)
            *f -= reference;
    for (auto& b : bc)
        if (b.kind == 0 || b.kind == 1)
            b.value -= reference;
    std::vector<detail::LocalMfd> local;
    local.reserve(nc);
    ThermalResult result;
    result.k_used = p.k;
    for (std::size_t c = 0; c < nc; ++c) {
        auto op = detail::build_local_mfd(g, c, p.k[c], p.stabilization);
        if (!op)
            return op.error();
        if (op.value().condition > result.max_local_condition) {
            result.max_local_condition = op.value().condition;
            result.max_condition_gid = g.cv_elem_gid[c];
        }
        result.gamma_floor_count += op.value().gamma_floor_count;
        local.push_back(std::move(op.value()));
    }
    try {
        detail::index(nf);
        detail::index(nc);
        std::vector<PetscInt> number(nf, -1), counts(static_cast<std::size_t>(ranks));
        PetscInt unknowns = 0;
        for (int r = 0; r < ranks; ++r)
            for (std::size_t f = 0; f < nf; ++f)
                if (bc[f].kind != 0 && g.cv_rank[g.faces[f].lo] == r) {
                    number[f] = unknowns++;
                    ++counts[r];
                }
        // Check all size conversions before entering collective PETSc allocation.
        detail::index(nf);
        detail::index(nc);
        const auto local_n = counts[rank];
        PetscInt begin = 0;
        for (int r = 0; r < rank; ++r)
            begin += counts[r];
        const auto end = begin + local_n;
        std::vector<std::set<PetscInt>> adjacency(static_cast<std::size_t>(local_n));
        for (std::size_t c = 0; c < nc; ++c)
            for (auto i = g.cv_face_off[c]; i < g.cv_face_off[c + 1]; ++i) {
                auto row = number[g.cv_faces[i]];
                if (row < begin || row >= end)
                    continue;
                for (auto j = g.cv_face_off[c]; j < g.cv_face_off[c + 1]; ++j) {
                    auto col = number[g.cv_faces[j]];
                    if (col >= 0)
                        adjacency[row - begin].insert(col);
                }
            }
        std::vector<PetscInt> dn(static_cast<std::size_t>(local_n)), on(dn.size());
        for (PetscInt i = 0; i < local_n; ++i)
            for (auto j : adjacency[i]) {
                if (j >= begin && j < end)
                    ++dn[i];
                else
                    ++on[i];
            }
        auto check = [&](PetscErrorCode err, const char* name) {
            detail::collective_check(err, comm, name);
        };
        detail::Matrix matrix;
        detail::Vector rhs, x;
        detail::Krylov solver;
        if (unknowns > 0) {
            check(MatCreate(comm, &matrix.value), "MatCreate");
            check(MatSetSizes(matrix.value, local_n, local_n, unknowns, unknowns), "MatSetSizes");
            check(MatSetType(matrix.value, MATMPIAIJ), "MatSetType");
            check(MatMPIAIJSetPreallocation(matrix.value, 0, dn.data(), 0, on.data()),
                  "preallocation");
            check(MatSetOption(matrix.value, MAT_NEW_NONZERO_ALLOCATION_ERR, PETSC_TRUE),
                  "allocation option");
            PetscInt actual_begin = 0, actual_end = 0;
            check(MatGetOwnershipRange(matrix.value, &actual_begin, &actual_end), "ownership");
            if (actual_begin != begin || actual_end != end)
                throw std::runtime_error("INTERNAL: face ownership mismatch");
            check(VecCreateMPI(comm, local_n, unknowns, &rhs.value), "VecCreateMPI");
            check(VecDuplicate(rhs.value, &x.value), "VecDuplicate");
            PetscErrorCode assembly_error = 0;
            for (std::size_t c = 0; c < nc; ++c)
                if (g.cv_rank[c] == rank) {
                    const auto& op = local[c];
                    const auto n = op.b.size();
                    for (std::size_t i = 0; i < n; ++i) {
                        auto f = static_cast<std::size_t>(
                            g.cv_faces[g.cv_face_off[c] + static_cast<std::int64_t>(i)]);
                        auto row = number[f];
                        if (row < 0)
                            continue;
                        long double load = fixed[c]
                                               ? op.b[i] * *fixed[c]
                                               : op.b[i] * p.q_vol[c] * g.cv_volume[c] / op.alpha;
                        for (std::size_t j = 0; j < n; ++j) {
                            auto fj = static_cast<std::size_t>(
                                g.cv_faces[g.cv_face_off[c] + static_cast<std::int64_t>(j)]);
                            long double value =
                                op.S[i * n + j] + (fixed[c] ? op.b[i] * op.b[j] / op.alpha : 0);
                            auto col = number[fj];
                            if (col < 0)
                                load -= value * bc[fj].value;
                            else {
                                auto err = MatSetValue(matrix.value,
                                                       row,
                                                       col,
                                                       static_cast<PetscScalar>(value),
                                                       ADD_VALUES);
                                if (err)
                                    assembly_error = err;
                            }
                        }
                        auto err =
                            VecSetValue(rhs.value, row, static_cast<PetscScalar>(load), ADD_VALUES);
                        if (err)
                            assembly_error = err;
                    }
                }
            for (std::size_t f = 0; f < nf; ++f)
                if (number[f] >= 0 && g.cv_rank[g.faces[f].lo] == rank) {
                    PetscErrorCode err = 0;
                    if (bc[f].kind == 1) {
                        auto ha = static_cast<PetscScalar>(bc[f].h * g.faces[f].area);
                        err = MatSetValue(matrix.value, number[f], number[f], ha, ADD_VALUES);
                        if (err)
                            assembly_error = err;
                        err = VecSetValue(rhs.value,
                                          number[f],
                                          ha * static_cast<PetscScalar>(bc[f].value),
                                          ADD_VALUES);
                    } else if (bc[f].kind == 2)
                        err = VecSetValue(rhs.value,
                                          number[f],
                                          static_cast<PetscScalar>(bc[f].value * g.faces[f].area),
                                          ADD_VALUES);
                    if (err)
                        assembly_error = err;
                }
            check(assembly_error, "local assembly");
            check(MatAssemblyBegin(matrix.value, MAT_FINAL_ASSEMBLY), "MatAssemblyBegin");
            check(MatAssemblyEnd(matrix.value, MAT_FINAL_ASSEMBLY), "MatAssemblyEnd");
            check(VecAssemblyBegin(rhs.value), "VecAssemblyBegin");
            check(VecAssemblyEnd(rhs.value), "VecAssemblyEnd");
            check(KSPCreate(comm, &solver.value), "KSPCreate");
            check(KSPSetOperators(solver.value, matrix.value, matrix.value), "operators");
            check(KSPSetType(solver.value, KSPCG), "KSPCG");
            PC pc = nullptr;
            check(KSPGetPC(solver.value, &pc), "KSPGetPC");
            check(PCSetType(pc, p.preconditioner == "jacobi" ? PCJACOBI : PCGAMG), "PCSetType");
            check(KSPSetTolerances(solver.value, p.rtol, 1e-50, PETSC_DEFAULT, p.max_iter),
                  "tolerances");
            check(KSPSolve(solver.value, rhs.value, x.value), "KSPSolve");
            KSPConvergedReason reason;
            PetscInt its = 0;
            PetscReal residual = 0;
            check(KSPGetConvergedReason(solver.value, &reason), "reason");
            check(KSPGetIterationNumber(solver.value, &its), "iterations");
            check(KSPGetResidualNorm(solver.value, &residual), "residual");
            result.iterations = static_cast<int>(its);
            result.residual = residual;
            if (reason <= 0) {
                std::ostringstream s;
                s << "INTERNAL: KSP nonconvergence iterations=" << its << " residual=" << residual;
                return mesh::Error{"fvm/thermal", s.str()};
            }
        }
        Dense lambda(nf);
        for (std::size_t f = 0; f < nf; ++f)
            if (number[f] < 0)
                lambda[f] = bc[f].value;
        if (unknowns > 0) {
            detail::Scatter gather;
            detail::Vector all;
            check(VecScatterCreateToAll(x.value, &gather.value, &all.value), "gather create");
            check(VecScatterBegin(gather.value, x.value, all.value, INSERT_VALUES, SCATTER_FORWARD),
                  "gather begin");
            check(VecScatterEnd(gather.value, x.value, all.value, INSERT_VALUES, SCATTER_FORWARD),
                  "gather end");
            const PetscScalar* values = nullptr;
            check(VecGetArrayRead(all.value, &values), "read solution");
            for (std::size_t f = 0; f < nf; ++f)
                if (number[f] >= 0)
                    lambda[f] = PetscRealPart(values[number[f]]);
            check(VecRestoreArrayRead(all.value, &values), "restore solution");
        }
        result.T_face.resize(nf);
        for (std::size_t f = 0; f < nf; ++f)
            result.T_face[f] = static_cast<double>(lambda[f] + reference);
        result.T_cell.resize(nc);
        result.face_flux.resize(nf);
        std::vector<std::array<long double, 2>> flux(nf);
        Dense sums(nc), abs_sums(nc);
        long double largest = 0;
        for (std::size_t c = 0; c < nc; ++c) {
            const auto& op = local[c];
            const auto n = op.b.size();
            Dense lc(n);
            for (std::size_t i = 0; i < n; ++i)
                lc[i] = lambda[g.cv_faces[g.cv_face_off[c] + static_cast<std::int64_t>(i)]];
            long double u = fixed[c] ? *fixed[c] : lc[0];
            if (!fixed[c]) {
                long double value = p.q_vol[c] * g.cv_volume[c];
                for (std::size_t i = 0; i < n; ++i)
                    value += op.b[i] * (lc[i] - lc[0]);
                u += value / op.alpha;
            }
            result.T_cell[c] = static_cast<double>(u + reference);
            for (auto& v : lc)
                v = u - v;
            auto q = op.apply(lc);
            for (std::size_t i = 0; i < n; ++i) {
                auto f = static_cast<std::size_t>(
                    g.cv_faces[g.cv_face_off[c] + static_cast<std::int64_t>(i)]);
                flux[f][g.faces[f].lo == static_cast<std::int64_t>(c) ? 0 : 1] = q[i];
                sums[c] += q[i];
                abs_sums[c] += std::abs(q[i]);
                largest = std::max(largest, std::abs(q[i]));
            }
        }
        long double in = 0, out = 0;
        auto ledger = [&](long double value) {
            in += std::max(value, 0.L);
            out += std::max(-value, 0.L);
        };
        for (std::size_t c = 0; c < nc; ++c) {
            long double s = p.q_vol[c] * g.cv_volume[c];
            ledger(s);
            if (fixed[c])
                ledger(sums[c] - s);
            else
                result.cell_balance_max =
                    std::max(result.cell_balance_max,
                             static_cast<double>(std::abs(sums[c] - s) /
                                                 std::max({abs_sums[c], std::abs(s), 1e-300L})));
        }
        for (std::size_t f = 0; f < nf; ++f) {
            result.face_flux[f] = static_cast<double>(flux[f][0]);
            if (g.faces[f].hi < 0)
                ledger(-flux[f][0]);
            else {
                const auto denominator =
                    std::max({std::abs(flux[f][0]), std::abs(flux[f][1]), 1e-300L * largest});
                // Define the indeterminate 0/0 jump as zero for an entirely zero-flux solve.
                const auto jump =
                    denominator > 0 ? std::abs(flux[f][0] + flux[f][1]) / denominator : 0;
                result.face_jump_max = std::max(result.face_jump_max, static_cast<double>(jump));
            }
        }
        result.heat_in = static_cast<double>(in);
        result.heat_out = static_cast<double>(out);
        result.imbalance = static_cast<double>(std::abs(in - out) / std::max(in + out, 1e-300L));
        // Original hex vertex order; constituent elements preserve merged incidence.
        const auto nv = m.vertices.size() / 3;
        Dense weight(nv), sum(nv), bw(nv), bsum(nv), dw(nv), dsum(nv);
        constexpr int vertices[6][4] = {
            {0, 4, 7, 3}, {1, 2, 6, 5}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 3, 2, 1}, {4, 5, 6, 7}};
        for (std::size_t c = 0; c < nc; ++c) {
            std::set<std::size_t> incident;
            auto add_parent = [&](std::int64_t element) {
                if (element < 0 || static_cast<std::size_t>(element) >= m.hex.size() / 8)
                    throw std::runtime_error("INPUT_INVALID: invalid CV parent");
                for (int j = 0; j < 8; ++j) {
                    auto v = m.hex[8 * element + j];
                    if (v < 0 || static_cast<std::size_t>(v) >= nv)
                        throw std::runtime_error("INPUT_INVALID: invalid parent vertex");
                    incident.insert(static_cast<std::size_t>(v));
                }
            };
            if (g.constituent_off.size() == nc + 1) {
                for (auto i = g.constituent_off[c]; i < g.constituent_off[c + 1]; ++i)
                    add_parent(g.constituent_elem.at(static_cast<std::size_t>(i)));
            } else {
                add_parent(g.cv_elem[c]);
            }
            for (auto v : incident) {
                weight[v] += g.cv_volume[c];
                sum[v] += g.cv_volume[c] * result.T_cell[c];
            }
        }
        for (std::size_t f = 0; f < nf; ++f) {
            const auto& face = g.faces[f];
            if (face.hi >= 0 || face.side < 0 || face.side >= 6)
                continue;
            const auto e = face.parent_elem >= 0 ? face.parent_elem : g.cv_elem[face.lo];
            for (int j : vertices[face.side]) {
                auto v = m.hex[8 * e + j];
                if (bc[f].kind == 0) {
                    dw[v] += face.area;
                    dsum[v] += face.area * result.T_face[f];
                } else {
                    bw[v] += face.area;
                    bsum[v] += face.area * result.T_face[f];
                }
            }
        }
        result.T_vertex.resize(nv);
        for (std::size_t v = 0; v < nv; ++v)
            result.T_vertex[v] = static_cast<double>(dw[v] > 0       ? dsum[v] / dw[v]
                                                     : bw[v] > 0     ? bsum[v] / bw[v]
                                                     : weight[v] > 0 ? sum[v] / weight[v]
                                                                     : reference);
        return result;
    } catch (const std::exception& e) {
        return mesh::Error{"fvm/thermal", e.what()};
    }
}
} // namespace chipcae::solvers::fvm
