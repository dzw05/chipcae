#include "chipcae/mesh/cut/detail/outline_nonconvex.hpp"
#include "fvm_test_util.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <petscksp.h>
#include <random>
#include <regex>

namespace {
namespace fs = std::filesystem;
namespace fv = fvm_test::fv;
namespace mesh = fvm_test::mesh;
using fvm_test::near;
using fvm_test::require;
using fvm_test::take;
constexpr double rtol = 1e-8;

int rank() {
    int value = 0;
    MPI_Comm_rank(PETSC_COMM_WORLD, &value);
    return value;
}
void together(bool ok, const std::string& message) {
    int local = ok ? 1 : 0, all = 0;
    MPI_Allreduce(&local, &all, 1, MPI_INT, MPI_MIN, PETSC_COMM_WORLD);
    require(all != 0, message);
}
void check(PetscErrorCode code, const std::string& operation) {
    together(code == PETSC_SUCCESS, operation + " failed, PETSc code=" + std::to_string(code));
}
void broadcast(std::string& value) {
    int length = static_cast<int>(value.size());
    MPI_Bcast(&length, 1, MPI_INT, 0, PETSC_COMM_WORLD);
    value.resize(static_cast<std::size_t>(length));
    MPI_Bcast(value.data(), length, MPI_CHAR, 0, PETSC_COMM_WORLD);
}
struct UnsupportedFormat : std::runtime_error {
    using std::runtime_error::runtime_error;
};
// Root-only I/O and parsing must finish (or fail on every rank) before PETSc collectives.
template <class F>
void root_io(F&& action) {
    std::string error;
    int unsupported = 0;
    if (rank() == 0) {
        try {
            action();
        } catch (const UnsupportedFormat& e) {
            error = e.what();
            unsupported = 1;
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    broadcast(error);
    MPI_Bcast(&unsupported, 1, MPI_INT, 0, PETSC_COMM_WORLD);
    if (unsupported)
        throw UnsupportedFormat(error);
    require(error.empty(), error);
}

template <class T, PetscErrorCode (*Destroy)(T*)>
struct Handle {
    T value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value)
            Destroy(&value);
    }
};
using Matrix = Handle<Mat, MatDestroy>;
using Vector = Handle<::Vec, VecDestroy>;
using Solver = Handle<KSP, KSPDestroy>;
using Viewer = Handle<PetscViewer, PetscViewerDestroy>;

struct CaseFiles {
    fs::path directory;
    PetscOptions options = nullptr;
    CaseFiles() {
        std::string path;
        root_io([&] {
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            std::random_device random;
            for (int attempt = 0; attempt < 100; ++attempt) {
                auto candidate =
                    fs::temp_directory_path() /
                    ("chipcae-ksp-" + std::to_string(stamp) + "-" + std::to_string(random()));
                if (fs::create_directory(candidate)) {
                    path = candidate.string();
                    return;
                }
            }
            throw std::runtime_error("cannot create unique test directory");
        });
        broadcast(path);
        directory = path;
        check(PetscOptionsCreate(&options), "PetscOptionsCreate");
        check(PetscOptionsPush(options), "PetscOptionsPush");
        for (const auto& [option, file] : std::vector<std::pair<std::string, std::string>>{
                 {"ksp_view_mat", "A"},
                 {"ksp_view_rhs", "b"},
                 {"ksp_view_solution", "x"},
                 {"ksp_converged_reason", "reason.txt"},
                 {"ksp_view", "view.txt"}}) {
            const auto type = file.ends_with(".txt") ? "ascii:" : "binary:";
            set(option, type + (directory / file).string());
        }
    }
    void set(const std::string& key, const std::string& value) {
        check(PetscOptionsSetValue(options, ("-fvm_thermal_" + key).c_str(), value.c_str()), key);
    }
    ~CaseFiles() {
        // solve_thermal's KSP and viewers, and our reload objects, have already died.
        PetscOptionsPop();
        PetscOptionsDestroy(&options);
        MPI_Barrier(PETSC_COMM_WORLD);
        if (rank() == 0) {
            std::error_code error;
            fs::remove_all(directory, error);
            if (error)
                std::cerr << "temporary directory cleanup failed: " << error.message() << '\n';
        }
        MPI_Barrier(PETSC_COMM_WORLD);
    }
};

struct Fixture {
    mesh::HexMeshArrays mesh;
    fv::FvGrid grid;
    fv::ThermalProblem problem;
};
Fixture fixture(bool cut, const std::string& pc) {
    int ranks = 1;
    MPI_Comm_size(PETSC_COMM_WORLD, &ranks);
    auto y = fvm_test::uniform(cut ? 8 : 16);
    for (auto& v : y)
        v *= .2;
    auto m = fvm_test::boxes(
        {fvm_test::uniform(cut ? 24 : 48), y, {0, .025, .05, .075, .1}}, false, ranks);
    mesh::Footprint outline;
    outline.box = {0, 0, 1, .2};
    if (cut)
        outline.polygon = {{0, .02}, {.9, 0}, {1, .02}, {1, .2}, {0, .2}};
    auto clipped = take(mesh::cut::detail::apply_outline_cut_general(
                            m, outline, mesh::cut::CutOptions{}, mesh::cut::CutGeometry{}),
                        "production outline cut");
    auto g = take(fv::build_grid(m, clipped ? &*clipped : nullptr), "build thermal guard grid");
    auto p = fvm_test::problem(g, {1, 1, 1}, pc);
    // Repository library cards at T_ref=300 K: copper=394, bt_hl832ns_eq=0.6 W/(m K).
    // Synthetic stack convention: copper below z=.05, BT above; two cells per layer.
    // elastic_accuracy_util.prepare_case uses synthetic a=2 below b=4, not library cards.
    // This library-card stack is explicit; it does not claim the original CLI provenance.
    for (std::size_t c = 0; c < g.cv_volume.size(); ++c) {
        const double k = g.cv_centroid[c][2] < .05 ? 394 : .6;
        p.k[c] = {k, k, k};
    }
    p.rtol = rtol;
    p.max_iter = 500;
    p.T_ref = 300;
    auto robin = fvm_test::dirichlet("face:zmax", 0);
    robin.kind = fv::ThermalBc::Kind::Robin;
    robin.h = 20;
    robin.T_inf = 400;
    p.bcs = {fvm_test::dirichlet("face:zmin", 300), robin};
    return {std::move(m), std::move(g), std::move(p)};
}
auto solve(const Fixture& f) {
    return fv::solve_thermal(f.grid, f.mesh, f.problem, chipcae::Communicator::world());
}

std::uint32_t read_int(std::ifstream& stream) {
    std::array<unsigned char, 4> bytes{};
    stream.read(reinterpret_cast<char*>(bytes.data()), 4);
    require(bool(stream), "truncated PETSc binary header");
    std::uint32_t value = 0;
    for (auto byte : bytes)
        value = (value << 8) | byte;
    return value;
}
int records(const fs::path& file, bool matrix) {
    std::ifstream stream(file, std::ios::binary);
    require(bool(stream), "missing PETSc export: " + file.filename().string());
    if (sizeof(PetscInt) != 4 || sizeof(PetscScalar) != 8 || PetscDefined(USE_COMPLEX))
        throw UnsupportedFormat("SKIP: binary oracle requires 32-bit PetscInt and real doubles");
    const auto size = fs::file_size(file);
    std::uintmax_t offset = 0;
    int count = 0;
    while (offset < size) {
        stream.seekg(static_cast<std::streamoff>(offset));
        const auto classid = read_int(stream);
        const auto expected = matrix ? 1211216U : 1211214U;
        if (classid != expected)
            throw UnsupportedFormat("SKIP: unsupported PETSc binary classid=" +
                                    std::to_string(classid) + " in " + file.filename().string());
        const std::uintmax_t rows = read_int(stream);
        std::uintmax_t length = 8 + rows * 8;
        if (matrix) {
            const auto columns = read_int(stream);
            const std::uintmax_t nonzeros = read_int(stream);
            require(rows > 0 && columns > 0 && nonzeros <= rows * columns,
                    "invalid exported matrix dimensions");
            length = 16 + rows * 4 + nonzeros * 12;
        }
        require(length <= size - offset, "truncated PETSc binary record");
        offset += length;
        ++count;
    }
    require(count > 0, "empty PETSc export: " + file.filename().string());
    return count;
}
std::string read_text(const fs::path& file) {
    std::ifstream stream(file);
    require(bool(stream), "missing PETSc export: " + file.filename().string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void load_matrix(const fs::path& path, int count, Matrix& matrix) {
    Viewer viewer;
    check(PetscViewerBinaryOpen(
              PETSC_COMM_WORLD, path.string().c_str(), FILE_MODE_READ, &viewer.value),
          "open matrix export");
    check(MatCreate(PETSC_COMM_WORLD, &matrix.value), "MatCreate");
    check(MatSetType(matrix.value, MATAIJ), "MatSetType");
    for (int i = 0; i < count; ++i)
        check(MatLoad(matrix.value, viewer.value), "MatLoad record " + std::to_string(i));
}
void load_vector(const fs::path& path, int count, Vector& vector) {
    Viewer viewer;
    check(PetscViewerBinaryOpen(
              PETSC_COMM_WORLD, path.string().c_str(), FILE_MODE_READ, &viewer.value),
          "open vector export");
    check(VecCreate(PETSC_COMM_WORLD, &vector.value), "VecCreate");
    check(VecSetType(vector.value, VECMPI), "VecSetType");
    for (int i = 0; i < count; ++i)
        check(VecLoad(vector.value, viewer.value), "VecLoad record " + std::to_string(i));
}
struct Exported {
    Matrix A;
    Vector b, x;
    explicit Exported(const CaseFiles& files) {
        std::array<int, 3> counts{};
        root_io([&] {
            counts = {records(files.directory / "A", true),
                      records(files.directory / "b", false),
                      records(files.directory / "x", false)};
            require(counts[0] == counts[1] && counts[1] == counts[2],
                    "matrix/RHS/solution export record counts disagree");
        });
        MPI_Bcast(counts.data(), 3, MPI_INT, 0, PETSC_COMM_WORLD);
        // Each viewer may contain several solves after restarts; keep the final records.
        load_matrix(files.directory / "A", counts[0], A);
        load_vector(files.directory / "b", counts[1], b);
        load_vector(files.directory / "x", counts[2], x);
    }
    double residual() const {
        Vector r;
        check(VecDuplicate(b.value, &r.value), "VecDuplicate residual oracle");
        check(MatMult(A.value, x.value, r.value), "MatMult residual oracle");
        check(VecAYPX(r.value, -1, b.value), "b-Ax residual oracle");
        PetscReal numerator = 0, denominator = 0;
        check(VecNorm(r.value, NORM_2, &numerator), "norm b-Ax");
        check(VecNorm(b.value, NORM_2, &denominator), "norm b");
        return denominator > 0 ? numerator / denominator : numerator;
    }
};
void converged_text(const CaseFiles& files) {
    root_io([&] {
        const auto reason = read_text(files.directory / "reason.txt");
        const auto last = reason.rfind("Linear ");
        require(last != std::string::npos, "missing final KSP convergence reason: " + reason);
        const auto final = reason.substr(last);
        require(final.find("CONVERGED_") != std::string::npos &&
                    final.find("DIVERGED_") == std::string::npos,
                "final KSP reason must be positive: " + final);
        const auto view = read_text(files.directory / "view.txt");
        // GAMG prints nested coarse/grid KSPs too. Inspect the final outer solver's
        // own header, ending before its PC view, rather than a nested KSP norm.
        const auto final_view = view.rfind("KSP Object: (fvm_thermal_)");
        require(final_view != std::string::npos, "missing final fvm_thermal_ KSP view");
        const auto pc_view = view.find("PC Object:", final_view);
        const auto outer = view.substr(final_view, pc_view - final_view);
        require(outer.find("UNPRECONDITIONED norm type for convergence test") != std::string::npos,
                "final KSP must use UNPRECONDITIONED norm type for convergence test");
    });
}
void accepted(const fv::ThermalResult& result, const Exported& exported) {
    const auto rho = exported.residual();
    together(std::isfinite(rho) && rho <= rtol,
             "independent true relative residual must be finite and <= rtol");
    // Synchronize numerical assertion failures before the following text-file collectives.
    std::string error;
    try {
        near(result.residual,
             rho,
             0,
             rho > 0 ? 1e-6 : 0,
             "ThermalResult::residual must be true relative residual (including exact zero)");
    } catch (const std::exception& e) {
        error = e.what();
    }
    together(error.empty(), error.empty() ? "residual mismatch on another rank" : error);
    together(!result.T_cell.empty(), "successful thermal result must contain cell temperatures");
    if (rank() == 0)
        std::cout << std::setprecision(17) << "iterations=" << result.iterations
                  << " rho_test=" << rho
                  << " T_max=" << *std::max_element(result.T_cell.begin(), result.T_cell.end())
                  << '\n';
}
double diagnostic_real(const std::string& token) {
    std::size_t consumed = 0;
    const auto value = std::stod(token, &consumed);
    require(consumed == token.size() && std::isfinite(value) && value >= 0,
            "diagnostic must contain a finite nonnegative number: " + token);
    return value;
}
int failure(const mesh::Expected<fv::ThermalResult>& result,
            const CaseFiles& files,
            const std::string& norm,
            int cap) {
    together(!result, "solve must return error without ThermalResult");
    const auto& error = result.error();
    std::string canonical_path = error.path, canonical_reason = error.reason;
    broadcast(canonical_path);
    broadcast(canonical_reason);
    together(error.path == canonical_path && error.reason == canonical_reason,
             "all ranks must return identical full error diagnostics");
    require(error.path == "fvm/thermal", "convergence error path: " + error.path);
    if (rank() == 0)
        std::cout << "observed error: " << error.reason << '\n';
    require(
        error.reason.starts_with("INTERNAL: SOLVER_NOT_CONVERGED (KSP nonconvergence) field=T "),
        "missing structured SOLVER_NOT_CONVERGED field=T: " + error.reason);
    // The public diagnostic is ordered and complete; field presence alone can hide
    // a stale absolute/preconditioned residual or a nonnumeric placeholder.
    std::smatch fields;
    require(std::regex_match(
                error.reason,
                fields,
                std::regex("INTERNAL: SOLVER_NOT_CONVERGED \\(KSP nonconvergence\\) field=T "
                           "reason=(-?[0-9]+) iterations=([0-9]+) true_residual=([^ ]+) "
                           "rtol=([^ ]+) norm_type=([a-z]+)")),
            "incorrect structured diagnostic field order or values: " + error.reason);
    const int reason = std::stoi(fields[1].str());
    require(reason != 0, "failure must report a completed KSP reason: " + error.reason);
    const int its = std::stoi(fields[2].str());
    require(its >= 0 && its <= cap, "total iteration budget exceeded: " + error.reason);
    const auto rho = diagnostic_real(fields[3].str());
    const auto target = diagnostic_real(fields[4].str());
    near(target, rtol, 0, 0, "diagnostic rtol must equal the requested global target");
    require(fields[5].str() == norm, "diagnostic must report effective norm: " + error.reason);
    require(reason < 0 || rho > target,
            "positive-reason rejection must explain an excessive true residual: " + error.reason);
    Exported exported(files);
    const auto expected = exported.residual();
    together(std::isfinite(expected), "independent rejected residual must be finite");
    near(rho,
         expected,
         0,
         expected > 0 ? 1e-6 : 0,
         "diagnostic true_residual must match independent final A/b/x residual");
    return reason;
}
void convergence(bool cut, const std::string& pc, bool mpi = false) {
    CaseFiles files;
    auto f = fixture(cut, pc);
    auto result = solve(f);
    together(bool(result), "thermal fixture must converge");
    Exported exported(files);
    if (rank() == 0)
        std::cout << "geometry=" << (cut ? "cut24x8" : "strip48x16") << " pc=" << pc << ' ';
    accepted(result.value(), exported);
    converged_text(files);
    if (mpi) {
        auto bits = std::bit_cast<std::uint64_t>(result.value().residual);
        auto canonical = bits;
        MPI_Bcast(&canonical, 1, MPI_UINT64_T, 0, PETSC_COMM_WORLD);
        together(bits == canonical, "residual must be bitwise identical across ranks");
    }
}
void capped(bool options) {
    CaseFiles files;
    auto f = fixture(true, "jacobi");
    if (options)
        files.set("ksp_max_it", "3");
    else
        f.problem.max_iter = 3;
    require(failure(solve(f), files, "unpreconditioned", 3) == -3,
            "iteration cap must report KSP_DIVERGED_ITS (-3)");
}

struct OldResult {
    int iterations;
    double rho;
};
OldResult old_criterion(Exported& exported, const std::string& pc, double target = rtol) {
    Solver solver;
    check(KSPCreate(PETSC_COMM_WORLD, &solver.value), "old KSPCreate");
    check(KSPSetOperators(solver.value, exported.A.value, exported.A.value), "old operators");
    check(KSPSetType(solver.value, KSPCG), "old CG");
    PC preconditioner = nullptr;
    check(KSPGetPC(solver.value, &preconditioner), "old KSPGetPC");
    check(PCSetType(preconditioner, pc == "gamg" ? PCGAMG : PCJACOBI), "old identical PC");
    check(KSPSetNormType(solver.value, KSP_NORM_PRECONDITIONED), "old preconditioned norm");
    check(KSPSetTolerances(solver.value, target, 1e-50, PETSC_DEFAULT, 500), "old tolerances");
    check(KSPSetInitialGuessNonzero(solver.value, PETSC_FALSE), "old zero initial guess");
    check(VecSet(exported.x.value, 0), "old VecSet zero");
    check(KSPSolve(solver.value, exported.b.value, exported.x.value), "old KSPSolve");
    KSPConvergedReason reason{};
    PetscInt iterations = 0;
    check(KSPGetConvergedReason(solver.value, &reason), "old reason");
    check(KSPGetIterationNumber(solver.value, &iterations), "old iterations");
    together(reason > 0, "old criterion reference must claim convergence");
    const auto rho = exported.residual();
    if (rank() == 0)
        std::cout << "old criterion pc=" << pc << " reason=" << static_cast<int>(reason)
                  << " its_old=" << iterations << " rho_old=" << std::setprecision(17) << rho
                  << '\n';
    return {static_cast<int>(iterations), rho};
}
void false_green() {
    OldResult cut_gamg{};
    bool any = false;
    for (bool cut : {true, false})
        for (const std::string pc : {"gamg", "jacobi"}) {
            CaseFiles files;
            auto f = fixture(cut, pc);
            together(bool(solve(f)), "export fixture must converge before old-criterion reference");
            Exported exported(files);
            if (rank() == 0)
                std::cout << "reference geometry=" << (cut ? "cut" : "strip") << ' ';
            auto old = old_criterion(exported, pc);
            any |= old.rho > rtol;
            if (cut && pc == "gamg")
                cut_gamg = old;
        }
    require(any, "ESCALATE: none of the four old-criterion fixtures has rho_old > rtol");
    require(cut_gamg.rho > rtol,
            "ESCALATE: cut/GAMG must establish rho_old > rtol before injecting old norm");
    CaseFiles injected;
    injected.set("ksp_norm_type", "preconditioned");
    auto f = fixture(true, "gamg");
    auto result = solve(f);
    if (result) {
        Exported exported(injected);
        accepted(result.value(), exported);
        require(result.value().iterations > cut_gamg.iterations,
                "old-norm injection must count additional restart iterations");
        if (rank() == 0)
            std::cout << "preconditioned injection: restarted and passed independent residual\n";
    } else {
        // A positive reason with an excessive true residual is also a valid rejection.
        failure(result, injected, "preconditioned", f.problem.max_iter);
        if (rank() == 0)
            std::cout << "preconditioned injection: rejected: " << result.error().reason << '\n';
    }
}
void restart_budget() {
    auto f = fixture(true, "gamg");
    OldResult old{};
    {
        CaseFiles reference;
        together(bool(solve(f)), "restart-budget reference must export a converged system");
        Exported exported(reference);
        old = old_criterion(exported, "gamg");
    }
    together(old.rho > rtol,
             "ESCALATE: cut/GAMG must establish a false green before capped restart injection");
    // Leave exactly one iteration after the old criterion first claims convergence.
    // Reusing the full prefixed max_it on restart would exceed this total budget.
    const int cap = old.iterations + 1;
    CaseFiles injected;
    injected.set("ksp_norm_type", "preconditioned");
    injected.set("ksp_max_it", std::to_string(cap));
    auto result = solve(f);
    if (result) {
        Exported exported(injected);
        accepted(result.value(), exported);
        require(result.value().iterations > old.iterations && result.value().iterations <= cap,
                "successful restart must count all iterations and respect the prefixed total cap");
    } else {
        failure(result, injected, "preconditioned", cap);
    }
}
void restart_target() {
    auto f = fixture(true, "gamg");
    OldResult old{};
    {
        CaseFiles reference;
        together(bool(solve(f)), "restart-target reference must export a converged system");
        Exported exported(reference);
        old = old_criterion(exported, "gamg", 9e-9);
    }
    together(old.rho > rtol,
             "ESCALATE: tighter old criterion must establish a false green before restart");
    CaseFiles injected;
    injected.set("ksp_norm_type", "preconditioned");
    injected.set("ksp_rtol", "9e-9");
    auto result = solve(f);
    together(bool(result), "restart must reach the requested global target");
    Exported exported(injected);
    accepted(result.value(), exported);
    together(result.value().iterations > old.iterations, "old-norm injection must restart");
    root_io([&] {
        const auto view = read_text(injected.directory / "view.txt");
        const auto first = view.find("KSP Object: (fvm_thermal_)");
        const auto last = view.rfind("KSP Object: (fvm_thermal_)");
        require(first != std::string::npos && last > first, "missing initial/restarted KSP views");
        auto tolerance = [&](std::size_t start) {
            const auto outer = view.substr(start, view.find("PC Object:", start) - start);
            std::smatch fields;
            require(std::regex_search(outer, fields, std::regex("tolerances: relative=([^,]+),")),
                    "missing outer KSP relative tolerance");
            return diagnostic_real(fields[1].str());
        };
        // PETSc's ASCII view prints tolerances with %g (six significant digits).
        near(tolerance(first), 9e-9, 0, 1e-5, "prefixed tolerance must apply to initial solve");
        near(tolerance(last) * old.rho,
             rtol,
             0,
             1e-5,
             "restarted KSP must target problem.rtol times the original RHS norm");
    });
}
void divergence() {
    CaseFiles files;
    // The brief's deterministic alternative to divtol injection.
    files.set("pc_type", "none");
    files.set("ksp_max_it", "1");
    auto f = fixture(true, "gamg");
    require(failure(solve(f), files, "unpreconditioned", 1) < 0,
            "no-PC one-iteration injection must report a negative KSP reason");
}
void zero_rhs() {
    CaseFiles files;
    auto f = fixture(true, "jacobi");
    f.problem.bcs[1].T_inf = 300;
    auto result = solve(f);
    together(bool(result), "zero RHS with remaining unknowns must succeed");
    require(f.grid.faces.size() > f.grid.cv_volume.size(), "zero RHS fixture has face unknowns");
    near(result.value().residual, 0, 0, 0, "zero RHS residual");
    require(result.value().iterations == 0, "zero RHS needs zero iterations");
    for (const auto* temperatures :
         {&result.value().T_cell, &result.value().T_face, &result.value().T_vertex})
        for (auto temperature : *temperatures)
            near(temperature, 300, 1e-12, 0, "zero RHS constant temperature");
}
void all_dirichlet() {
    CaseFiles files;
    auto m = fvm_test::boxes({{0, 1}, {0, .2}, {0, .1}});
    auto g = take(fv::build_grid(m), "one-cell grid");
    auto p = fvm_test::problem(g);
    p.rtol = rtol;
    p.T_ref = 300;
    for (const std::string face : {"xmin", "xmax", "ymin", "ymax", "zmin", "zmax"})
        p.bcs.push_back(fvm_test::dirichlet("face:" + face, 300));
    auto result = fv::solve_thermal(g, m, p, chipcae::Communicator::world());
    together(bool(result), "all-six-faces Dirichlet must succeed");
    near(result.value().residual, 0, 0, 0, "no unknowns residual");
    require(result.value().iterations == 0, "no unknowns needs zero iterations");
    for (const auto* temperatures :
         {&result.value().T_cell, &result.value().T_face, &result.value().T_vertex})
        for (auto temperature : *temperatures)
            near(temperature, 300, 1e-12, 0, "all Dirichlet temperature");
    root_io([&] {
        require(fs::is_empty(files.directory), "unknowns=0 must not construct KSP/export files");
    });
}
} // namespace

int main(int argc, char** argv) {
    fvm_test::PetscSession petsc(argc, argv);
    fvm_test::Suite suite;
    int unsupported = 0;
    const bool mpi_only = std::any_of(
        argv + 1, argv + argc, [](const char* arg) { return std::string(arg) == "--mpi-only"; });
    if (mpi_only)
        check(PetscOptionsClearValue(nullptr, "--mpi-only"), "consume MPI test switch");
    int ranks = 1;
    MPI_Comm_size(PETSC_COMM_WORLD, &ranks);
    if (mpi_only && ranks != 2) {
        if (rank() == 0)
            std::cout << "SKIP: MPI acceptance requires exactly two ranks\n";
        return 77;
    }
    auto run = [&](const std::string& name, auto&& body) {
        suite.run(name, [&] {
            try {
                body();
            } catch (const UnsupportedFormat&) {
                ++unsupported;
                throw;
            }
        });
    };
    if (mpi_only) {
        run("np2 cut/Jacobi global true residual and bitwise rank agreement",
            [] { convergence(true, "jacobi", true); });
        run("np2 iteration cap returns identical full errors", [] { capped(false); });
    } else {
        for (bool cut : {true, false})
            for (const std::string pc : {"jacobi", "gamg"})
                run(std::string(cut ? "cut24x8 " : "strip48x16 ") + pc +
                        " independent residual, final reason and norm",
                    [&] { convergence(cut, pc); });
        run("problem max_iter=3 rejects without fields", [] { capped(false); });
        run("prefixed max_it=3 remains authoritative", [] { capped(true); });
        run("old positive convergence cannot bypass true residual guard", false_green);
        run("old-norm restart respects prefixed total iteration budget", restart_budget);
        run("restart restores global target after a tighter initial tolerance", restart_target);
        run("injected no-PC one-iteration divergence is structured", divergence);
        run("zero RHS with unknowns keeps zero diagnostics", zero_rhs);
        run("all Dirichlet bypasses KSP and exports", all_dirichlet);
    }
    const int status = suite.finish();
    return unsupported > 0 && unsupported == suite.failed ? 77 : status;
}
