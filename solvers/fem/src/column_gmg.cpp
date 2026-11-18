#include "chipcae/solvers/fem/column_gmg.hpp"

#include "chipcae/mesh/background/column_hierarchy.hpp"
#include "column_gmg_checks.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <numeric>
#include <petscblaslapack.h>
#include <petscksp.h>
#include <sstream>
#include <type_traits>
#include <utility>

namespace chipcae::fem {
namespace {
using Clock = std::chrono::steady_clock;
using detail::verify_layout;
using detail::verify_operator;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error("column_gmg: " + message);
}

template <class To, class From>
To index_cast(From value) {
    require(std::in_range<To>(value), "index overflow");
    return static_cast<To>(value);
}

// MFEM's CSR and lattice maps use int indices, even when PETSc uses wider indices.
int product(std::size_t a, std::size_t b) {
    require(b == 0 || a <= static_cast<std::size_t>(std::numeric_limits<int>::max()) / b,
            "layout size overflow");
    return index_cast<int>(a * b);
}

void check(PetscErrorCode code) {
    if (code)
        throw std::runtime_error("column_gmg: PETSc error " + std::to_string(code));
}

[[noreturn]] void unsupported(const std::string& reason) {
    throw ColumnGmgUnsupported("column_gmg unsupported: " + reason);
}

void validate_axis(const std::vector<double>& axis) {
    require(axis.size() >= 2, "invalid hierarchy axis");
    for (std::size_t i = 0; i < axis.size(); ++i)
        require(std::isfinite(axis[i]) && (i == 0 || axis[i] > axis[i - 1]),
                "invalid hierarchy axis");
}

std::vector<double> lattice_axis(const std::vector<double>& axis) {
    std::vector<double> result;
    result.reserve(2 * axis.size() - 1);
    for (std::size_t i = 1; i < axis.size(); ++i) {
        result.push_back(axis[i - 1]);
        result.push_back(axis[i - 1] + .5 * (axis[i] - axis[i - 1]));
    }
    result.push_back(axis.back());
    return result;
}

// Validate scope before accessing the FE space or building any DOF maps.
void validate_mesh(const mesh::HexMeshArrays& m, bool has_cut, int np) {
    if (has_cut || !m.pending_cut.empty())
        unsupported("cut mesh");
    if (np != 1)
        unsupported("np=" + std::to_string(np));
    const auto& report = m.grid_report;
    require(report.levels > 0 && report.levels <= 31, "invalid hierarchy levels");
    if (report.levels == 1)
        unsupported("single level");
    require(report.level_axes.size() == static_cast<std::size_t>(report.levels),
            "invalid hierarchy axes");
    validate_axis(m.axes[2]);
    for (int k = 0; k < report.levels; ++k)
        for (int a = 0; a < 2; ++a) {
            const auto& axis = report.level_axes[k][a];
            validate_axis(axis);
            index_cast<int>(axis.size());
            if (k > 0) {
                const auto& coarse = report.level_axes[k - 1][a];
                require(axis.size() - 1 == 2 * (coarse.size() - 1), "invalid hierarchy refinement");
                for (std::size_t i = 0; i < coarse.size(); ++i)
                    require(axis[2 * i] == coarse[i], "invalid hierarchy grid lines");
            }
        }
    const auto& x = report.level_axes.back()[0];
    const auto& y = report.level_axes.back()[1];
    const auto& z = m.axes[2];
    const int nx = index_cast<int>(x.size() - 1), ny = index_cast<int>(y.size() - 1);
    const int nz = index_cast<int>(z.size() - 1), columns = product(nx, ny);
    const int cells = product(columns, nz);
    require(m.hex.size() % 8 == 0 && m.vertices.size() % 3 == 0 &&
                m.hex.size() / 8 == m.elem_gid.size() && m.attribute.size() == m.elem_gid.size() &&
                m.partitioning.size() == m.elem_gid.size(),
            "inconsistent mesh arrays");
    if (m.elem_gid.size() != static_cast<std::size_t>(cells))
        unsupported("non-regular column -1");
    const auto nx0 = index_cast<std::uint32_t>(report.level_axes[0][0].size() - 1);
    const auto ny0 = index_cast<std::uint32_t>(report.level_axes[0][1].size() - 1);
    std::vector<std::pair<int, int>> cell_xy(columns, {-1, -1});
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const auto d = mesh::dense_index(nx0, ny0, report.levels - 1, i, j);
            require(d < cell_xy.size() && cell_xy[d].first == -1, "invalid dense hierarchy");
            cell_xy[d] = {i, j};
        }
    std::vector<std::uint8_t> seen(cells, 0);
    std::int64_t offending = std::numeric_limits<std::int64_t>::max();
    const std::array<double, 3> tolerance{1e-12 * (x.back() - x.front()),
                                          1e-12 * (y.back() - y.front()),
                                          1e-12 * (z.back() - z.front())};
    for (std::size_t e = 0; e < m.elem_gid.size(); ++e) {
        const auto gid = m.elem_gid[e];
        if (gid < 0 || gid >= cells) {
            offending = std::min(offending, gid < 0 ? std::int64_t{-1} : gid / nz);
            continue;
        }
        const auto d = gid / nz;
        if (seen[gid]++)
            offending = std::min(offending, d);
        const auto [i, j] = cell_xy[d];
        require(i >= 0, "missing dense hierarchy index");
        const auto kz = gid % nz;
        for (int corner : {0, 6}) {
            const int v = m.hex[8 * e + corner];
            require(v >= 0 && static_cast<std::size_t>(v) < m.vertices.size() / 3,
                    "invalid mesh vertex index");
            const int hi = corner == 6 ? 1 : 0;
            const std::array<double, 3> expected{x[i + hi], y[j + hi], z[kz + hi]};
            for (int a = 0; a < 3; ++a)
                if (!std::isfinite(m.vertices[3 * static_cast<std::size_t>(v) + a]) ||
                    std::abs(m.vertices[3 * static_cast<std::size_t>(v) + a] - expected[a]) >
                        tolerance[a])
                    offending = std::min(offending, d);
        }
    }
    for (int gid = 0; gid < cells; ++gid)
        if (!seen[gid])
            offending = std::min(offending, static_cast<std::int64_t>(gid / nz));
    if (offending != std::numeric_limits<std::int64_t>::max())
        unsupported("non-regular column " + std::to_string(offending));
    if (report.void_cells != 0)
        unsupported("non-regular column -1");
}

struct Lattice {
    std::vector<std::pair<int, int>> xy;
    std::vector<int> block;
    explicit Lattice(const ColumnGmgLevel& level) {
        const int nx = index_cast<int>(level.x.size()), ny = index_cast<int>(level.y.size());
        block.resize(product(nx, ny));
        xy.reserve(block.size());
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                xy.emplace_back(i, j);
        std::sort(xy.begin(), xy.end(), [](auto a, auto b) {
            return mesh::morton2d(a.first, a.second) < mesh::morton2d(b.first, b.second);
        });
        for (std::size_t c = 0; c < xy.size(); ++c)
            block[xy[c].first + nx * xy[c].second] = index_cast<int>(c);
    }
};

int match(double value, const std::vector<double>& axis, double tolerance, int s) {
    const auto after = std::lower_bound(axis.begin(), axis.end(), value);
    std::size_t i = static_cast<std::size_t>(after - axis.begin());
    if (i == axis.size() || (i > 0 && std::abs(axis[i - 1] - value) < std::abs(axis[i] - value)))
        --i;
    require(std::isfinite(value) && std::abs(axis[i] - value) <= tolerance,
            "dof " + std::to_string(s) + " not on Q2 lattice");
    return index_cast<int>(i);
}

double lattice_tolerance(const std::vector<double>& axis) {
    double spacing = std::numeric_limits<double>::infinity();
    for (std::size_t i = 1; i < axis.size(); ++i)
        spacing = std::min(spacing, axis[i] - axis[i - 1]);
    return 1e-9 * spacing;
}

using Stencil = std::vector<std::pair<int, double>>;
std::vector<Stencil> interpolation(const std::vector<double>& coarse,
                                   const std::vector<double>& fine) {
    std::vector<Stencil> result(2 * fine.size() - 1);
    for (std::size_t f = 0; f < result.size(); ++f) {
        if (f % 4 == 0) {
            result[f].emplace_back(index_cast<int>(f / 2), 1.);
            continue;
        }
        const std::size_t e = f / 4;
        const double a = coarse[e], b = coarse[e + 1], c = a + .5 * (b - a);
        const double x =
            f % 2 == 0 ? fine[f / 2] : fine[f / 2] + .5 * (fine[f / 2 + 1] - fine[f / 2]);
        const std::array<double, 3> w{(x - c) * (x - b) / ((a - c) * (a - b)),
                                      (x - a) * (x - b) / ((c - a) * (c - b)),
                                      (x - a) * (x - c) / ((b - a) * (b - c))};
        for (int j = 0; j < 3; ++j)
            if (std::abs(w[j]) > 1e-14)
                result[f].emplace_back(index_cast<int>(2 * e + j), w[j]);
    }
    return result;
}

} // namespace
namespace detail {
void verify_layout(const ColumnGmgLevel& level, int slots, int rows) {
    const int blocks = product(level.x.size(), level.y.size());
    require(level.col_off.size() == static_cast<std::size_t>(blocks) + 1 &&
                level.col_off.front() == 0 && level.col_off.back() == rows &&
                level.col_dof.size() == static_cast<std::size_t>(rows) &&
                level.essential.size() == static_cast<std::size_t>(product(blocks, slots)),
            "operator/block layout mismatch");
    require(std::is_sorted(level.col_off.begin(), level.col_off.end()), "invalid block offsets");
    std::vector<std::uint8_t> seen(level.essential.size(), 0);
    for (const auto dof : level.col_dof) {
        require(dof >= 0 && static_cast<std::size_t>(dof) < seen.size() && !seen[dof],
                "invalid column DOF permutation");
        seen[dof] = 1;
    }
}
} // namespace detail
namespace {

template <class To, class From>
const To* csr_input(const From* source, std::size_t count, std::vector<To>& converted) {
    if constexpr (std::is_same_v<To, From>) {
        return source;
    } else {
        converted.resize(count);
        for (std::size_t i = 0; i < count; ++i) {
            if constexpr (std::is_integral_v<To> && std::is_integral_v<From>)
                converted[i] = index_cast<To>(source[i]);
            else
                converted[i] = source[i];
        }
        return converted.data();
    }
}

// PETSc copies CSR input, so equal MFEM/PETSc types need no temporary conversion buffers.
// After this call Mat owns its storage independently of both MFEM and the converted arrays.
void copy_csr(const mfem::SparseMatrix& a, Mat& mat) {
    require(a.Finalized(), "matrix is not finalized");
    const auto n = a.GetI()[a.Height()];
    const auto row_count = static_cast<std::size_t>(a.Height()) + 1;
    std::vector<PetscInt> converted_rows, converted_columns;
    std::vector<PetscScalar> converted_values;
    const auto* rows = csr_input(a.GetI(), row_count, converted_rows);
    const auto* columns = csr_input(a.GetJ(), index_cast<std::size_t>(n), converted_columns);
    const auto* values = csr_input(a.GetData(), index_cast<std::size_t>(n), converted_values);
    check(MatCreateSeqAIJ(PETSC_COMM_SELF,
                          index_cast<PetscInt>(a.Height()),
                          index_cast<PetscInt>(a.Width()),
                          0,
                          nullptr,
                          &mat));
    check(MatSeqAIJSetPreallocationCSR(mat, rows, columns, values));
}

mfem::SparseMatrix permute(const mfem::SparseMatrix& a, const std::vector<std::int64_t>& order) {
    const int n = a.Height();
    require(a.Finalized() && n == a.Width() && order.size() == static_cast<std::size_t>(n),
            "system dimensions do not match FE space");
    std::vector<int> inverse(n);
    for (int r = 0; r < n; ++r)
        inverse[index_cast<int>(order[r])] = r;
    const int nnz = a.GetI()[n];
    auto rows = std::make_unique<int[]>(static_cast<std::size_t>(n) + 1);
    auto columns = std::make_unique<int[]>(nnz);
    auto values = std::make_unique<double[]>(nnz);
    rows[0] = 0;
    std::vector<std::pair<int, double>> row;
    for (int r = 0; r < n; ++r) {
        const auto old = order[r];
        row.clear();
        for (int j = a.GetI()[old]; j < a.GetI()[old + 1]; ++j) {
            require(a.GetJ()[j] >= 0 && a.GetJ()[j] < n, "invalid system column");
            row.emplace_back(inverse[a.GetJ()[j]], a.GetData()[j]);
        }
        std::sort(row.begin(), row.end());
        int p = rows[r];
        for (const auto& [col, value] : row) {
            columns[p] = col;
            values[p++] = value;
        }
        rows[r + 1] = p;
    }
    mfem::SparseMatrix result(rows.get(), columns.get(), values.get(), n, n);
    rows.release();
    columns.release();
    values.release();
    return result;
}

} // namespace
namespace detail {
void verify_operator(Mat a, const ColumnGmgLevel& level, int k, bool positive) {
    PetscInt rows, columns;
    check(MatGetSize(a, &rows, &columns));
    require(rows == columns && rows == level.col_off.back(), "Galerkin layout mismatch");
    if (!positive)
        return;
    struct Diagonal {
        Vec value = nullptr;
        ~Diagonal() { VecDestroy(&value); }
    } diagonal;
    check(MatCreateVecs(a, nullptr, &diagonal.value));
    check(MatGetDiagonal(a, diagonal.value));
    PetscInt count;
    check(VecGetLocalSize(diagonal.value, &count));
    require(count == rows, "coarse diagonal size mismatch");
    const PetscScalar* values;
    check(VecGetArrayRead(diagonal.value, &values));
    const bool valid = std::all_of(values, values + count, [](PetscScalar value) {
        return std::isfinite(value) && value > 0;
    });
    check(VecRestoreArrayRead(diagonal.value, &values));
    if (!valid)
        throw std::runtime_error("column_gmg: coarse operator level " + std::to_string(k) +
                                 " not positive");
}
} // namespace detail
namespace {

struct ColumnBlocks {
    struct Block {
        PetscInt first = 0;
        PetscBLASInt n = 0;
        std::vector<PetscScalar> lu;
        std::vector<PetscBLASInt> pivots;
    };
    const ColumnGmgHierarchy& hierarchy;
    PC mg;
    int level;
    std::vector<Block> blocks;
    std::exception_ptr failure;

    ColumnBlocks(const ColumnGmgHierarchy& h, PC pc, int k) : hierarchy(h), mg(pc), level(k) {}

    std::string singular(std::size_t c) const {
        return "column_gmg: singular column block level " + std::to_string(level) + " block " +
               std::to_string(c);
    }

    void setup(PC pc) {
        // PCMG has already formed every Galerkin operator before setting up its smoothers.
        // Validate level zero here, before PCMG sets up the coarse LU factorization.
        if (level == 1) {
            KSP coarse;
            Mat a;
            check(PCMGGetCoarseSolve(mg, &coarse));
            check(KSPGetOperators(coarse, &a, nullptr));
            verify_operator(a, hierarchy.levels[0], 0, true);
        }
        Mat a;
        check(PCGetOperators(pc, &a, nullptr));
        const auto& layout = hierarchy.levels[level];
        verify_operator(a, layout, level, level < hierarchy.L);
        if (!blocks.empty())
            return;
        blocks.resize(layout.col_off.size() - 1);
        for (std::size_t c = 0; c < blocks.size(); ++c) {
            auto& block = blocks[c];
            block.first = index_cast<PetscInt>(layout.col_off[c]);
            block.n = index_cast<PetscBLASInt>(layout.col_off[c + 1] - layout.col_off[c]);
            if (block.n == 0)
                continue;
            block.lu.assign(static_cast<std::size_t>(block.n) * block.n, 0.);
            block.pivots.resize(block.n);
            for (PetscInt r = 0; r < block.n; ++r) {
                const PetscInt row = block.first + r;
                PetscInt nnz;
                const PetscInt* columns;
                const PetscScalar* values;
                check(MatGetRow(a, row, &nnz, &columns, &values));
                for (PetscInt j = 0; j < nnz; ++j)
                    if (columns[j] >= block.first && columns[j] < block.first + block.n)
                        block.lu[(columns[j] - block.first) * block.n + r] = values[j];
                check(MatRestoreRow(a, row, &nnz, &columns, &values));
            }
            PetscBLASInt info = 0;
            LAPACKgetrf_(&block.n, &block.n, block.lu.data(), &block.n, block.pivots.data(), &info);
            if (info != 0)
                throw std::runtime_error(singular(c));
        }
    }

    void apply(Vec rhs, Vec solution) {
        // VecCopy handles aliasing and leaves the PETSc arrays restored if LAPACK fails.
        check(VecCopy(rhs, solution));
        PetscScalar* values;
        check(VecGetArray(solution, &values));
        PetscBLASInt failure_info = 0;
        for (const auto& block : blocks) {
            if (block.n == 0)
                continue;
            const PetscBLASInt nrhs = 1;
            PetscBLASInt info = 0;
            LAPACKgetrs_("N",
                         &block.n,
                         &nrhs,
                         block.lu.data(),
                         &block.n,
                         block.pivots.data(),
                         values + block.first,
                         &block.n,
                         &info);
            if (info != 0) {
                failure_info = info;
                break;
            }
        }
        check(VecRestoreArray(solution, &values));
        require(failure_info == 0, "column block solve failed");
    }
};

PetscErrorCode setup_blocks(PC pc) noexcept {
    ColumnBlocks* context = nullptr;
    const auto code = PCShellGetContext(pc, &context);
    if (code)
        return code;
    try {
        context->setup(pc);
        return 0;
    } catch (...) {
        context->failure = std::current_exception();
        return PETSC_ERR_LIB;
    }
}

PetscErrorCode apply_blocks(PC pc, Vec rhs, Vec solution) noexcept {
    ColumnBlocks* context = nullptr;
    const auto code = PCShellGetContext(pc, &context);
    if (code)
        return code;
    try {
        context->apply(rhs, solution);
        return 0;
    } catch (...) {
        context->failure = std::current_exception();
        return PETSC_ERR_LIB;
    }
}

struct PetscResources {
    KSP ksp = nullptr;
    Mat a = nullptr;
    Vec rhs = nullptr, solution = nullptr;
    std::vector<Mat> interpolation;
    std::vector<std::unique_ptr<ColumnBlocks>> contexts;
    explicit PetscResources(int L) : interpolation(L, nullptr) {}
    ~PetscResources() {
        KSPDestroy(&ksp);
        MatDestroy(&a);
        for (auto& p : interpolation)
            MatDestroy(&p);
        VecDestroy(&rhs);
        VecDestroy(&solution);
        // Contexts outlive KSP/PC. Galerkin matrices belong exclusively to PCMG.
    }
    void checked(PetscErrorCode code) const {
        for (const auto& context : contexts)
            if (context->failure)
                std::rethrow_exception(context->failure);
        check(code);
    }
};
} // namespace

ColumnGmgHierarchy build_column_gmg_hierarchy(const mesh::HexMeshArrays& m,
                                              bool has_cut,
                                              int np,
                                              const mfem::FiniteElementSpace& fes,
                                              const mfem::Array<int>& essential_tdofs) {
    validate_mesh(m, has_cut, np);
    ColumnGmgHierarchy h;
    h.L = m.grid_report.levels - 1;
    h.ncomp = fes.GetVDim();
    require(h.ncomp == 1 || h.ncomp == 3, "expected scalar or three-component Q2 space");
    require(fes.GetConformingProlongation() == nullptr, "conforming DOF map is not identity");
    mfem::FiniteElementSpace coords(fes.GetMesh(), fes.FEColl(), 3, mfem::Ordering::byNODES);
    require(coords.GetNDofs() == fes.GetNDofs(), "coordinate scalar DOF count mismatch");
    const auto z = lattice_axis(m.axes[2]);
    h.nz_nodes = index_cast<int>(z.size());
    const int slots = product(z.size(), h.ncomp);
    h.levels.resize(h.L + 1);
    h.P.resize(h.L);
    std::vector<Lattice> lattices;
    lattices.reserve(h.L + 1);
    for (int k = 0; k <= h.L; ++k) {
        auto& level = h.levels[k];
        level.x = lattice_axis(m.grid_report.level_axes[k][0]);
        level.y = lattice_axis(m.grid_report.level_axes[k][1]);
        lattices.emplace_back(level);
        level.essential.assign(product(lattices.back().xy.size(), slots), 0);
    }
    auto& finest = h.levels.back();
    const auto& lattice = lattices.back();
    require(product(lattice.xy.size(), z.size()) == fes.GetNDofs(),
            "Q2 lattice scalar DOF count mismatch");
    require(finest.essential.size() == static_cast<std::size_t>(fes.GetVSize()),
            "Q2 lattice vector DOF count mismatch");
    require(fes.GetTrueVSize() == fes.GetVSize(), "true/vector DOF count mismatch");
    finest.col_dof.assign(finest.essential.size(), -1);
    std::vector<std::uint8_t> essential(fes.GetTrueVSize(), 0);
    for (int tdof : essential_tdofs) {
        require(tdof >= 0 && tdof < fes.GetTrueVSize(), "essential tdof out of range");
        essential[tdof] = 1;
    }
    mfem::GridFunction xyz(&coords);
    mfem::VectorFunctionCoefficient identity(3,
                                             [](const mfem::Vector& p, mfem::Vector& v) { v = p; });
    xyz.ProjectCoefficient(identity);
    const std::array<double, 3> tolerance{
        lattice_tolerance(finest.x), lattice_tolerance(finest.y), lattice_tolerance(z)};
    const int nx = index_cast<int>(finest.x.size());
    for (int s = 0; s < fes.GetNDofs(); ++s) {
        const int ix = match(xyz[coords.DofToVDof(s, 0)], finest.x, tolerance[0], s);
        const int iy = match(xyz[coords.DofToVDof(s, 1)], finest.y, tolerance[1], s);
        const int iz = match(xyz[coords.DofToVDof(s, 2)], z, tolerance[2], s);
        const int full = lattice.block[ix + nx * iy] * slots + iz * h.ncomp;
        require(finest.col_dof[full] == -1, "dof " + std::to_string(s) + " not on Q2 lattice");
        for (int c = 0; c < h.ncomp; ++c) {
            const int tdof = fes.DofToVDof(s, c);
            require(tdof >= 0 && tdof < fes.GetTrueVSize(), "component tdof out of range");
            finest.col_dof[full + c] = tdof;
            finest.essential[full + c] = essential[tdof];
        }
    }
    require(std::find(finest.col_dof.begin(), finest.col_dof.end(), -1) == finest.col_dof.end(),
            "dof -1 not on Q2 lattice");
    finest.col_off.resize(lattice.xy.size() + 1);
    for (std::size_t c = 0; c < finest.col_off.size(); ++c)
        finest.col_off[c] = static_cast<std::int64_t>(c) * slots;
    verify_layout(finest, slots, fes.GetTrueVSize());

    for (int k = h.L - 1; k >= 0; --k) {
        auto& coarse = h.levels[k];
        const auto& fine = h.levels[k + 1];
        const auto px =
            interpolation(m.grid_report.level_axes[k][0], m.grid_report.level_axes[k + 1][0]);
        const auto py =
            interpolation(m.grid_report.level_axes[k][1], m.grid_report.level_axes[k + 1][1]);
        const auto& coarse_lattice = lattices[k];
        const auto& fine_lattice = lattices[k + 1];
        const int coarse_nx = index_cast<int>(coarse.x.size());
        // Visit the operator rows in full-layout order, generating at most nine entries each.
        const auto visit = [&](auto emit) {
            for (std::size_t r = 0; r < fine.col_dof.size(); ++r) {
                const int full =
                    k + 1 == h.L ? index_cast<int>(r) : index_cast<int>(fine.col_dof[r]);
                if (fine.essential[full])
                    continue;
                const auto [ix, iy] = fine_lattice.xy[full / slots];
                for (const auto& [cx, wx] : px[ix])
                    for (const auto& [cy, wy] : py[iy]) {
                        const double w = wx * wy;
                        if (std::abs(w) <= 1e-14)
                            continue;
                        const int col =
                            coarse_lattice.block[cx + coarse_nx * cy] * slots + full % slots;
                        emit(index_cast<int>(r), col, w);
                    }
            }
        };
        coarse.essential.assign(coarse.essential.size(), 1);
        visit([&](int, int col, double) { coarse.essential[col] = 0; });
        std::vector<int> compact(coarse.essential.size(), -1);
        coarse.col_off.push_back(0);
        for (std::size_t c = 0; c < coarse_lattice.xy.size(); ++c) {
            for (int slot = 0; slot < slots; ++slot) {
                const auto full = c * slots + slot;
                if (!coarse.essential[full]) {
                    compact[full] = index_cast<int>(coarse.col_dof.size());
                    coarse.col_dof.push_back(index_cast<std::int64_t>(full));
                }
            }
            coarse.col_off.push_back(index_cast<std::int64_t>(coarse.col_dof.size()));
        }
        mfem::SparseMatrix p(index_cast<int>(fine.col_dof.size()),
                             index_cast<int>(coarse.col_dof.size()));
        visit([&](int r, int col, double w) {
            require(compact[col] >= 0, "empty retained interpolation column");
            p.Add(r, compact[col], w);
        });
        p.Finalize(0);
        h.P[k].Swap(p);
        verify_layout(coarse, slots, h.P[k].Width());
        verify_layout(fine, slots, h.P[k].Height());
    }
    return h;
}

ColumnGmgStats solve_column_gmg(const mesh::HexMeshArrays& m,
                                bool has_cut,
                                int np,
                                const mfem::FiniteElementSpace& fes,
                                const mfem::SparseMatrix& A,
                                const mfem::Array<int>& essential_tdofs,
                                const mfem::Vector& b,
                                mfem::Vector& x,
                                const ColumnGmgOptions& options) {
    const auto begin = Clock::now();
    const auto h = build_column_gmg_hierarchy(m, has_cut, np, fes, essential_tdofs);
    require(std::isfinite(options.rtol) && options.rtol > 0 && options.max_iter > 0 &&
                options.chebyshev_degree > 0 && std::isfinite(options.chebyshev_lower) &&
                options.chebyshev_lower > 0 && options.chebyshev_lower < 1.1 &&
                options.eig_iterations > 0,
            "invalid solver options");
    require(A.Height() == fes.GetTrueVSize() && A.Width() == A.Height() && b.Size() == A.Height(),
            "system dimensions do not match FE space");
    ColumnGmgStats stats;
    for (int k = 0; k <= h.L; ++k) {
        stats.level_rows.push_back(h.levels[k].col_off.back());
        if (k > 0)
            stats.level_blocks.push_back(index_cast<std::int64_t>(h.levels[k].col_off.size() - 1));
    }
    const double bnorm = b.Norml2();
    require(std::isfinite(bnorm), "nonfinite RHS");
    x.SetSize(b.Size());
    x = 0.;
    if (bnorm == 0.) {
        stats.setup_seconds = std::chrono::duration<double>(Clock::now() - begin).count();
        return stats;
    }
    const auto& order = h.levels.back().col_dof;
    mfem::Vector rhs(b.Size()), solution(b.Size());
    for (int r = 0; r < b.Size(); ++r)
        rhs[r] = b[index_cast<int>(order[r])];
    solution = 0.;
    PetscResources owned(h.L);
    {
        auto a = permute(A, order);
        copy_csr(a, owned.a);
    }
    for (int k = 0; k < h.L; ++k)
        copy_csr(h.P[k], owned.interpolation[k]);
    check(VecCreateSeqWithArray(
        PETSC_COMM_SELF, 1, index_cast<PetscInt>(b.Size()), rhs.GetData(), &owned.rhs));
    check(VecCreateSeqWithArray(
        PETSC_COMM_SELF, 1, index_cast<PetscInt>(b.Size()), solution.GetData(), &owned.solution));
    check(KSPCreate(PETSC_COMM_SELF, &owned.ksp));
    check(KSPSetOperators(owned.ksp, owned.a, owned.a));
    check(KSPSetType(owned.ksp, KSPCG));
    check(KSPSetNormType(owned.ksp, KSP_NORM_UNPRECONDITIONED));
    check(KSPSetTolerances(owned.ksp, options.rtol, 0., PETSC_DEFAULT, options.max_iter));
    PC mg;
    check(KSPGetPC(owned.ksp, &mg));
    check(PCSetType(mg, PCMG));
    check(PCMGSetLevels(mg, h.L + 1, nullptr));
    check(PCMGSetType(mg, PC_MG_MULTIPLICATIVE));
    check(PCMGSetCycleType(mg, PC_MG_CYCLE_V));
    check(PCMGSetGalerkin(mg, PC_MG_GALERKIN_BOTH));
    for (int k = 1; k <= h.L; ++k) {
        check(PCMGSetInterpolation(mg, k, owned.interpolation[k - 1]));
        KSP smoother, estimator;
        PC pc;
        check(PCMGGetSmoother(mg, k, &smoother));
        check(KSPSetType(smoother, KSPCHEBYSHEV));
        check(KSPSetTolerances(
            smoother, PETSC_DEFAULT, PETSC_DEFAULT, PETSC_DEFAULT, options.chebyshev_degree));
        check(KSPSetNormType(smoother, KSP_NORM_NONE));
        check(KSPSetConvergenceTest(smoother, KSPConvergedSkip, nullptr, nullptr));
        check(KSPChebyshevEstEigSet(smoother, 0, options.chebyshev_lower, 0, 1.1));
        check(KSPChebyshevEstEigGetKSP(smoother, &estimator));
        check(KSPSetTolerances(
            estimator, PETSC_DEFAULT, PETSC_DEFAULT, PETSC_DEFAULT, options.eig_iterations));
        check(KSPGetPC(smoother, &pc));
        check(PCSetType(pc, PCSHELL));
        auto context = std::make_unique<ColumnBlocks>(h, mg, k);
        auto* pointer = context.get();
        owned.contexts.push_back(std::move(context));
        check(PCShellSetContext(pc, pointer));
        check(PCShellSetSetUp(pc, setup_blocks));
        check(PCShellSetApply(pc, apply_blocks));
    }
    KSP coarse;
    PC coarse_pc;
    check(PCMGGetCoarseSolve(mg, &coarse));
    check(KSPSetType(coarse, KSPPREONLY));
    check(KSPGetPC(coarse, &coarse_pc));
    check(PCSetType(coarse_pc, PCLU));
    check(PCFactorSetMatSolverType(coarse_pc, MATSOLVERPETSC));
    check(PCFactorSetMatOrderingType(coarse_pc, MATORDERINGND));
    check(PCFactorSetShiftType(coarse_pc, MAT_SHIFT_NONZERO));
    owned.checked(KSPSetUp(owned.ksp));
    const auto solve_begin = Clock::now();
    stats.setup_seconds = std::chrono::duration<double>(solve_begin - begin).count();
    owned.checked(KSPSolve(owned.ksp, owned.rhs, owned.solution));
    stats.solve_seconds = std::chrono::duration<double>(Clock::now() - solve_begin).count();
    PetscInt iterations;
    KSPConvergedReason reason;
    check(KSPGetIterationNumber(owned.ksp, &iterations));
    check(KSPGetConvergedReason(owned.ksp, &reason));
    stats.iterations = index_cast<std::uint32_t>(iterations);
    for (int r = 0; r < b.Size(); ++r)
        x[index_cast<int>(order[r])] = solution[r];
    mfem::Vector residual(b.Size());
    A.Mult(x, residual);
    residual -= b;
    stats.relative_residual = residual.Norml2() / std::max(bnorm, 1.);
    if (reason != KSP_CONVERGED_RTOL && reason != KSP_CONVERGED_ATOL) {
        std::ostringstream message;
        message.precision(17);
        message << "column_gmg diverged: reason=" << KSPConvergedReasons[reason]
                << " iterations=" << iterations << " relative_residual=" << stats.relative_residual;
        throw std::runtime_error(message.str());
    }
    return stats;
}
} // namespace chipcae::fem
