#include "elastic_stencil.hpp"
#include "elastic_test_mesh.hpp"

#include <chrono>
#include <iomanip>
#include <petscksp.h>
#include <petsclog.h>
#include <sstream>
#include <sys/resource.h>

using namespace elastic_test;
namespace {
std::vector<V> affine(const std::vector<V>& points, const M3& a, const V& b = {}) {
    std::vector<V> u;
    for (const auto& x : points)
        u.push_back(add(mul(a, x), b));
    return u;
}
std::vector<V> face_points(const ElasticMesh& m) {
    std::vector<V> x;
    for (const auto& f : m.faces)
        x.push_back(f.centroid);
    return x;
}
std::vector<V> face_forces(const detail::ElasticAssembly& a, const std::vector<V>& u) {
    std::vector<V> t;
    for (const auto& f : a.faces) {
        V value = f.constant;
        for (const auto& b : f.blocks) {
            require(b.column >= 0 && static_cast<std::size_t>(b.column) < u.size(),
                    "face stencil column");
            value = add(value, mul(b.value, u[b.column]));
        }
        t.push_back(value);
    }
    return t;
}
detail::ElasticAssembly assembly(const ElasticMesh& m, const ElasticProblem& p) {
    auto a = detail::assemble_elastic(m, p, MPI_COMM_SELF);
    require(static_cast<bool>(a), a ? "" : "elastic assembly must succeed: " + a.error().reason);
    require(a.value().faces.size() == m.faces.size(),
            "one oriented traction operator per input face");
    require(a.value().rhs.size() == m.volume.size(), "assembled rhs length");
    return std::move(a.value());
}
void traction(const ElasticMesh& m,
              const ElasticProblem& p,
              const ElasticResult& r,
              double tol,
              bool embedded_only = false) {
    const auto t = face_forces(assembly(m, p), r.U_cell);
    double stiffness = 0, strain = 0;
    for (std::size_t c = 0; c < p.C.size(); ++c) {
        for (double v : p.C[c])
            stiffness = std::max(stiffness, std::abs(v));
        for (double v : p.alpha[c])
            strain = std::max(strain, std::abs(v * p.dT[c]));
    }
    require(stiffness * strain > 0, "nonzero traction normalization");
    for (std::size_t f = 0; f < m.faces.size(); ++f)
        if (!embedded_only || m.is_embedded[f])
            require(norm(t[f]) <= tol * stiffness * strain * m.faces[f].area,
                    "integrated traction / (max|C| max|alpha*dT| area) on face " +
                        std::to_string(f));
}
void expansion(const ElasticMesh& m, double tol = 1e-10, bool cut = false) {
    const auto p = problem(m);
    const auto r = solve(m, p, MPI_COMM_SELF);
    check_output(m, p, r);
    const double s = 80e-5;
    const M3 a{s, 0, 0, 0, s, 0, 0, 0, s};
    const V b = scale(center(m), -s);
    const double atol = tol * length(m) * s;
    field_near(r.U_cell, affine(m.centroid, a, b), atol, 0, "free isotropic expansion cells");
    field_near(r.U_face,
               affine(face_points(m), a, b),
               atol,
               0,
               "boundary-consistent affine reconstruction");
    near(r.warpage_pv, 0, 4 * atol, 0, "affine top plane removed");
    V translation{}, rotation{};
    const V c = center(m);
    for (std::size_t i = 0; i < r.U_cell.size(); ++i) {
        translation = add(translation, r.U_cell[i]);
        rotation = add(rotation, cross(sub(m.centroid[i], c), r.U_cell[i]));
    }
    require(norm(translation) <= atol * static_cast<double>(m.volume.size()),
            "zero arithmetic translation gauge");
    require(norm(rotation) <= atol * length(m) * static_cast<double>(m.volume.size()),
            "zero rotation gauge");
    traction(m, p, r, tol, cut);
}
void affine_patch(
    const ElasticMesh& m,
    const M3& a = {0.002, -0.0007, 0.0011, 0.0003, -0.0013, 0.0009, -0.0004, 0.0006, 0.0008},
    Material mat = orthotropic()) {
    auto p = problem(m, mat, 37);
    const V b{0.0017, -0.0008, 0.0002};
    prescribe(p, m, a, b);
    const auto r = solve(m, p, MPI_COMM_SELF);
    check_output(m, p, r);
    field_near(r.U_cell, affine(m.centroid, a, b), 0, 1e-10, "orthotropic affine cell patch");
    field_near(r.U_face, affine(face_points(m), a, b), 0, 1e-10, "affine face interpolation");
}
void zero(const ElasticMesh& m, bool fixed) {
    auto p = problem(m, orthotropic(), 0);
    if (fixed) {
        p.support = ElasticProblem::Support::Fixed;
        p.fixed_side = {0};
    }
    const auto r = solve(m, p, MPI_COMM_SELF);
    check_output(m, p, r);
    // Exact zero is explicitly required by the brief; relative tolerance cannot test it.
    require(max_norm(r.U_cell) == 0 && max_norm(r.U_face) == 0,
            "zero load gives exact zero fields");
    require(r.warpage_pv == 0 && r.U_mag_max == 0, "zero load gives exact zero summaries");
    require(r.iterations == 0 || r.iterations == 1, "zero load takes 0 or 1 iterations");
}
void geometry() {
    const auto m = clipped(4);
    const double delta = 1.55 - 1.4;
    const double missing = delta * delta * delta / (6 * 0.35 * 0.2);
    const double volume = std::accumulate(m.volume.begin(), m.volume.end(), 0.0);
    near(volume, 1 - missing, 1e-13, 0, "exact clipped tetrahedron volume");
    V first{};
    for (std::size_t c = 0; c < m.volume.size(); ++c)
        first = add(first, scale(m.centroid[c], m.volume[c]));
    const V removed_centroid{1 - delta / 4, 1 - delta / (4 * 0.35), 1 - delta / (4 * 0.2)};
    const V expected = sub({0.5, 0.5, 0.5}, scale(removed_centroid, missing));
    require(norm(sub(first, expected)) <= 2e-13, "exact clipped first moments");
    std::vector<V> closure(m.volume.size());
    std::vector<double> area(m.volume.size());
    bool embedded = false, nonorthogonal = false;
    for (std::size_t i = 0; i < m.faces.size(); ++i) {
        const auto& f = m.faces[i];
        const V av = scale(f.normal, f.area);
        closure[f.lo] = add(closure[f.lo], av);
        area[f.lo] += f.area;
        embedded = embedded || m.is_embedded[i];
        if (f.hi >= 0) {
            closure[f.hi] = sub(closure[f.hi], av);
            area[f.hi] += f.area;
            const auto d = sub(m.centroid[f.hi], m.centroid[f.lo]);
            require(dot(d, f.normal) > 0, "positive projected internal distance");
            nonorthogonal = nonorthogonal || norm(cross(d, f.normal)) > 1e-6;
        } else {
            require(dot(sub(f.centroid, m.centroid[f.lo]), f.normal) > 0,
                    "positive projected boundary distance");
        }
    }
    for (std::size_t i = 0; i < closure.size(); ++i)
        require(norm(closure[i]) <= 1e-12 * area[i], "clipped cell area-vector closure");
    require(embedded && nonorthogonal, "fixture exercises embedded and nonorthogonal open faces");
}
void matrix_invariants() {
    const auto m = grid(3, 3, 3);
    auto p = problem(m);
    const auto a = assembly(m, p);
    const auto n = m.volume.size();
    std::map<std::pair<std::int64_t, std::int64_t>, M3> blocks;
    double anorm2 = 0;
    for (const auto& b : a.blocks) {
        require(b.row >= 0 && b.column >= 0 && static_cast<std::size_t>(b.row) < n &&
                    static_cast<std::size_t>(b.column) < n,
                "matrix block indices use input cells");
        auto& v = blocks[{b.row, b.column}];
        for (int j = 0; j < 9; ++j)
            v[j] += b.value[j];
    }
    for (const auto& [key, b] : blocks) {
        (void)key;
        for (double v : b)
            anorm2 += v * v;
    }
    require(anorm2 > 0, "nonempty elastic stiffness");
    const double anorm = std::sqrt(anorm2);
    double defect2 = 0;
    std::vector<M3> column_sums(n);
    for (const auto& [key, b] : blocks) {
        const auto it = blocks.find({key.second, key.first});
        const M3 transpose = it == blocks.end() ? M3{} : it->second;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                const double d = b[3 * i + j] - transpose[3 * j + i];
                defect2 += d * d;
                column_sums[key.second][3 * i + j] += b[3 * i + j];
            }
        if (it == blocks.end())
            for (double v : b)
                defect2 += v * v;
    }
    std::cout << "实测 matrix symmetry ||A-A^T||F/||A||F=" << std::sqrt(defect2) / anorm << '\n';
    double cancel2 = 0;
    for (const auto& b : column_sums)
        for (double v : b)
            cancel2 += v * v;
    require(std::sqrt(cancel2) / anorm <= 1e-13, "internal face forces cancel in assembled rows");
    V rhs_sum{};
    double rhs_norm = 0;
    for (const auto& b : a.rhs) {
        rhs_sum = add(rhs_sum, b);
        rhs_norm += dot(b, b);
    }
    require(norm(rhs_sum) <= 1e-13 * std::sqrt(rhs_norm), "thermal internal forces cancel");
    const V c = center(m);
    for (int mode = 0; mode < 6; ++mode) {
        V e{};
        e[mode % 3] = 1;
        std::vector<V> q(n), aq(n);
        double qnorm2 = 0, aqnorm2 = 0;
        for (std::size_t i = 0; i < n; ++i) {
            q[i] = mode < 3 ? e : cross(e, sub(m.centroid[i], c));
            qnorm2 += dot(q[i], q[i]);
        }
        for (const auto& [key, b] : blocks)
            aq[key.first] = add(aq[key.first], mul(b, q[key.second]));
        for (const auto& v : aq)
            aqnorm2 += dot(v, v);
        require(std::sqrt(aqnorm2 / qnorm2) / anorm <= 1e-12,
                "independent rigid mode annihilated: " + std::to_string(mode));
    }
    // Sum n x forces also vanishes: rotations are left modes, so a right-only
    // RHS projection must not mask an incompatible nonsymmetric assembly.
    for (int mode = 0; mode < 3; ++mode) {
        V e{};
        e[mode] = 1;
        std::vector<V> qa(n);
        double qnorm2 = 0, qa2 = 0, qb = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const V q = cross(e, sub(m.centroid[i], c));
            qnorm2 += dot(q, q);
            qb += dot(q, a.rhs[i]);
        }
        for (const auto& [key, b] : blocks) {
            const V q = cross(e, sub(m.centroid[key.first], c));
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i)
                    qa[key.second][j] += q[i] * b[3 * i + j];
        }
        for (const auto& v : qa)
            qa2 += dot(v, v);
        require(std::sqrt(qa2 / qnorm2) / anorm <= 1e-12, "transpose rigid-mode compatibility");
        require(std::abs(qb) <= 1e-12 * std::sqrt(qnorm2 * rhs_norm),
                "thermal RHS compatible with left rigid modes");
    }
}
void shear(int mode) {
    auto m = grid(3, 3, 3);
    auto p = problem(m, orthotropic(), 0);
    M3 a{};
    // Engineering gamma_yz, gamma_xz, gamma_xy = 0.002, independently.
    const std::array<std::array<int, 2>, 3> pair{{{1, 2}, {0, 2}, {0, 1}}};
    const auto ij = pair[mode];
    a[3 * ij[0] + ij[1]] = a[3 * ij[1] + ij[0]] = 0.001;
    prescribe(p, m, a);
    const auto r = solve(m, p, MPI_COMM_SELF);
    field_near(r.U_cell, affine(m.centroid, a), 0, 1e-10, "engineering shear displacement");
    const auto t = face_forces(assembly(m, p), r.U_cell);
    const double tau = p.C[0][6 * (3 + mode) + (3 + mode)] * 0.002;
    M3 sigma{};
    sigma[3 * ij[0] + ij[1]] = sigma[3 * ij[1] + ij[0]] = tau;
    for (std::size_t f = 0; f < m.faces.size(); ++f)
        require(norm(sub(t[f], scale(mul(sigma, m.faces[f].normal), m.faces[f].area))) <=
                    1e-10 * tau * m.faces[f].area,
                "engineering shear traction, component " + std::to_string(mode));
}
void invalid(const std::function<void(ElasticMesh&, ElasticProblem&)>& mutate) {
    auto m = grid(2, 2, 2);
    auto p = problem(m);
    mutate(m, p);
    PetscLogEvent event = 0;
    require(PetscLogEventGetId("KSPSolve", &event) == PETSC_SUCCESS, "find PETSc KSP log event");
    PetscEventPerfInfo before{}, after{};
    require(PetscLogEventGetPerfInfo(PETSC_DETERMINE, event, &before) == PETSC_SUCCESS,
            "KSP event before");
    const auto r = solve_elastic(m, p, MPI_COMM_SELF);
    require(PetscLogEventGetPerfInfo(PETSC_DETERMINE, event, &after) == PETSC_SUCCESS,
            "KSP event after");
    require(!r, "malformed input must return failure");
    require(r.error().reason.find("INPUT_INVALID") != std::string::npos,
            "expected INPUT_INVALID, got: " + r.error().reason);
    require(after.count == before.count, "invalid input rejected before KSPSolve");
}
void validation(Suite& s) {
    using Mutate = std::function<void(ElasticMesh&, ElasticProblem&)>;
    const std::vector<std::pair<std::string, Mutate>> cases{
        {"preconditioner", [](auto&, auto& p) { p.preconditioner = "direct"; }},
        {"mixed sides", [](auto&, auto& p) { p.fixed_side = {0}; }},
        {"mixed layers", [](auto&, auto& p) { p.fixed_layer = {0}; }},
        {"mixed prescribed", [](auto&, auto& p) { p.prescribed = {{0, {}}}; }},
        {"unmatched side",
         [](auto&, auto& p) {
             p.support = ElasticProblem::Support::Fixed;
             p.fixed_side = {6};
         }},
        {"unmatched valid side",
         [](auto& m, auto& p) {
             for (auto& f : m.faces)
                 if (f.side == 0)
                     f.side = 2;
             p.support = ElasticProblem::Support::Fixed;
             p.fixed_side = {0, 1};
         }},
        {"unmatched layer",
         [](auto&, auto& p) {
             p.support = ElasticProblem::Support::Fixed;
             p.fixed_layer = {0, 123};
         }},
        {"negative lo", [](auto& m, auto&) { m.faces[0].lo = -1; }},
        {"large hi",
         [](auto& m, auto&) { m.faces[0].hi = static_cast<std::int64_t>(m.volume.size()); }},
        {"invalid negative hi", [](auto& m, auto&) { m.faces[0].hi = -2; }},
        {"self face", [](auto& m, auto&) { m.faces[0].hi = m.faces[0].lo; }},
        {"rank outside comm", [](auto& m, auto&) { m.rank[0] = 1; }},
        {"negative rank", [](auto& m, auto&) { m.rank[0] = -1; }},
        {"duplicate gid", [](auto& m, auto&) { m.gid[1] = m.gid[0]; }},
        {"missing centroid", [](auto& m, auto&) { m.centroid.pop_back(); }},
        {"missing volume", [](auto& m, auto&) { m.volume.pop_back(); }},
        {"missing gid", [](auto& m, auto&) { m.gid.pop_back(); }},
        {"missing layer", [](auto& m, auto&) { m.layer.pop_back(); }},
        {"missing rank", [](auto& m, auto&) { m.rank.pop_back(); }},
        {"missing embedded flag", [](auto& m, auto&) { m.is_embedded.pop_back(); }},
        {"missing C", [](auto&, auto& p) { p.C.pop_back(); }},
        {"missing alpha", [](auto&, auto& p) { p.alpha.pop_back(); }},
        {"missing dT", [](auto&, auto& p) { p.dT.pop_back(); }},
        {"zero volume", [](auto& m, auto&) { m.volume[0] = 0; }},
        {"negative volume", [](auto& m, auto&) { m.volume[0] = -1; }},
        {"zero area", [](auto& m, auto&) { m.faces[0].area = 0; }},
        {"negative area", [](auto& m, auto&) { m.faces[0].area = -1; }},
        {"nonunit normal", [](auto& m, auto&) { m.faces[0].normal = {2, 0, 0}; }},
        {"zero projected distance",
         [](auto& m, auto&) { m.faces[0].centroid = m.centroid[m.faces[0].lo]; }},
        {"reversed normal",
         [](auto& m, auto&) { m.faces[0].normal = scale(m.faces[0].normal, -1); }},
        {"singular C", [](auto&, auto& p) { p.C[0] = {}; }},
        {"indefinite C", [](auto&, auto& p) { p.C[0][0] = -1; }},
        {"indefinite positive diagonals",
         [](auto&, auto& p) { p.C[0][1] = p.C[0][6] = 2 * p.C[0][0]; }},
        {"asymmetric C", [](auto&, auto& p) { p.C[0][1] *= 1.5; }},
        {"rtol zero", [](auto&, auto& p) { p.rtol = 0; }},
        {"rtol negative", [](auto&, auto& p) { p.rtol = -1; }},
        {"max_iter zero", [](auto&, auto& p) { p.max_iter = 0; }},
        {"max_iter negative", [](auto&, auto& p) { p.max_iter = -1; }},
        {"prescribed negative index",
         [](auto&, auto& p) {
             p.support = ElasticProblem::Support::Fixed;
             p.prescribed = {{-1, {}}};
         }},
        {"prescribed large index",
         [](auto& m, auto& p) {
             p.support = ElasticProblem::Support::Fixed;
             p.prescribed = {{static_cast<std::int64_t>(m.faces.size()), {}}};
         }},
        {"prescribed internal",
         [](auto& m, auto& p) {
             p.support = ElasticProblem::Support::Fixed;
             for (std::size_t i = 0; i < m.faces.size(); ++i)
                 if (m.faces[i].hi >= 0) {
                     p.prescribed = {{static_cast<std::int64_t>(i), {}}};
                     break;
                 }
         }},
        {"prescribed embedded",
         [](auto& m, auto& p) {
             m.is_embedded[0] = 1;
             m.faces[0].side = -1;
             p.support = ElasticProblem::Support::Fixed;
             p.prescribed = {{0, {}}};
         }},
        {"conflicting prescribed",
         [](auto&, auto& p) {
             p.support = ElasticProblem::Support::Fixed;
             p.prescribed = {{0, {}}, {0, {1, 0, 0}}};
         }},
        {"fixed conflicts prescribed", [](auto&, auto& p) {
             p.support = ElasticProblem::Support::Fixed;
             p.fixed_side = {0};
             p.prescribed = {{0, {1, 0, 0}}};
         }}};
    for (const auto& [name, f] : cases)
        s.run("INPUT_INVALID " + name, [&] { invalid(f); });
    for (double bad :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        const std::string suffix = std::isnan(bad) ? " NaN" : " Inf";
        const std::vector<std::pair<std::string, Mutate>> nonfinite{
            {"volume", [=](auto& m, auto&) { m.volume[0] = bad; }},
            {"centroid", [=](auto& m, auto&) { m.centroid[0][0] = bad; }},
            {"area", [=](auto& m, auto&) { m.faces[0].area = bad; }},
            {"normal", [=](auto& m, auto&) { m.faces[0].normal[0] = bad; }},
            {"face centroid", [=](auto& m, auto&) { m.faces[0].centroid[0] = bad; }},
            {"z_top", [=](auto& m, auto&) { m.z_top = bad; }},
            {"C", [=](auto&, auto& p) { p.C[0][0] = bad; }},
            {"alpha", [=](auto&, auto& p) { p.alpha[0][0] = bad; }},
            {"dT", [=](auto&, auto& p) { p.dT[0] = bad; }},
            {"rtol", [=](auto&, auto& p) { p.rtol = bad; }},
            {"prescribed", [=](auto&, auto& p) {
                 p.support = ElasticProblem::Support::Fixed;
                 p.prescribed = {{0, {bad, 0, 0}}};
             }}};
        for (const auto& [name, f] : nonfinite)
            s.run("INPUT_INVALID " + name + suffix, [&] { invalid(f); });
    }
}
void more_validation(Suite& s) {
    s.run("INPUT_INVALID empty mesh", [] {
        invalid([](auto& m, auto& p) {
            m = {};
            p.C.clear();
            p.alpha.clear();
            p.dT.clear();
        });
    });
    s.run("INPUT_INVALID invalid side",
          [] { invalid([](auto& m, auto&) { m.faces[0].side = 7; }); });
    s.run("INPUT_INVALID negative layer target", [] {
        invalid([](auto&, auto& p) {
            p.support = ElasticProblem::Support::Fixed;
            p.fixed_layer = {-1};
        });
    });
}
void supports() {
    const auto m = grid(3, 2, 2);
    for (bool layer : {false, true}) {
        auto p = problem(m);
        p.support = ElasticProblem::Support::Fixed;
        if (layer)
            p.fixed_layer = {0};
        else
            p.fixed_side = {0};
        auto explicit_p = p;
        explicit_p.fixed_layer.clear();
        explicit_p.fixed_side.clear();
        for (std::size_t f = 0; f < m.faces.size(); ++f)
            if (m.faces[f].hi < 0 && (layer ? m.layer[m.faces[f].lo] == 0 : m.faces[f].side == 0))
                explicit_p.prescribed.push_back({static_cast<std::int64_t>(f), {}});
        const auto expanded = solve(m, p, MPI_COMM_SELF);
        const auto direct = solve(m, explicit_p, MPI_COMM_SELF);
        check_output(m, p, expanded);
        check_output(m, explicit_p, direct);
        field_near(expanded.U_cell,
                   direct.U_cell,
                   0,
                   1e-10,
                   "Fixed expansion matches explicit boundary faces");
        for (const auto& bc : explicit_p.prescribed)
            require(expanded.U_face[bc.face] == V{}, "expanded support returns exact zero");
        explicit_p.prescribed.push_back(explicit_p.prescribed.front());
        const auto duplicate = solve(m, explicit_p, MPI_COMM_SELF);
        field_near(
            duplicate.U_cell, direct.U_cell, 0, 1e-10, "identical prescribed values deduplicated");
    }
}
void failed_ksp() {
    auto m = grid(12, 5, 6);
    auto p = problem(m, orthotropic());
    p.support = ElasticProblem::Support::Fixed;
    p.fixed_side = {0};
    p.max_iter = 1;
    p.rtol = 1e-15;
    for (std::size_t i = 0; i < p.dT.size(); ++i)
        p.dT[i] = 30 + 20 * std::sin(1.3 * static_cast<double>(i));
    const auto r = solve_elastic(m, p, MPI_COMM_SELF);
    require(!r, "one GMRES iteration must not return unconverged fields as success");
    const auto why = r.error().reason;
    require(why.find("iter") != std::string::npos && why.find("1") != std::string::npos &&
                why.find("residual") != std::string::npos,
            "solver failure includes iteration count and physical residual: " + why);
    require(why.find("INPUT_INVALID") == std::string::npos,
            "valid difficult problem is a solver failure");
}
void compare_results(const ElasticResult& a, const ElasticResult& b, double tol) {
    field_near(a.U_cell, b.U_cell, 1e-20, tol, "full cells");
    field_near(a.U_face, b.U_face, 1e-20, tol, "full reconstructed faces");
    require(a.top_samples.size() == b.top_samples.size(), "full top sample count");
    std::vector<V> top_a, top_b;
    for (std::size_t i = 0; i < a.top_samples.size(); ++i) {
        require(a.top_samples[i][0] == b.top_samples[i][0] &&
                    a.top_samples[i][1] == b.top_samples[i][1],
                "ordered sample coordinates");
        top_a.push_back({0, 0, a.top_samples[i][2]});
        top_b.push_back({0, 0, b.top_samples[i][2]});
    }
    field_near(top_a, top_b, 1e-20, tol, "full top displacements");
    require(a.face_prescribed == b.face_prescribed, "all face flags identical");
    require(a.warpage_method == b.warpage_method && a.U_mag_method == b.U_mag_method,
            "methods identical");
    near(a.warpage_pv, b.warpage_pv, 1e-16, tol, "warpage agreement");
    near(a.U_mag_max, b.U_mag_max, 1e-20, tol, "magnitude agreement");
}
void preconditioners() {
    const auto m = grid(6, 3, 4);
    auto p = problem(m, orthotropic());
    p.support = ElasticProblem::Support::Fixed;
    p.fixed_side = {0};
    for (std::size_t i = 0; i < p.C.size(); ++i) {
        const auto mat = laminate(i % 2 ? 0.3 : 0);
        p.C[i] = mat.C;
        p.alpha[i] = mat.alpha;
        p.dT[i] *= 1 + 0.1 * m.centroid[i][0];
    }
    const auto jacobi = solve(m, p, MPI_COMM_SELF);
    p.preconditioner = "gamg";
    const auto gamg = solve(m, p, MPI_COMM_SELF);
    check_output(m, p, gamg);
    compare_results(gamg, jacobi, 1e-7);
}
void cut_support() {
    const auto m = clipped(4);
    auto p = problem(m);
    p.support = ElasticProblem::Support::Fixed;
    p.fixed_side = {0};
    const auto r = solve(m, p, MPI_COMM_SELF);
    check_output(m, p, r);
    require(r.U_mag_max > 0, "cut cantilever responds to thermal loading");
    traction(m, p, r, 1e-8, true);
}
void uncut_equivalence() {
    const auto plain = grid(3, 3, 3), cut = clipped(3, 2);
    auto p = problem(plain), q = problem(cut);
    const M3 a{0.001, 0.002, 0, -0.001, 0.003, 0.002, 0, 0.001, 0.002};
    prescribe(p, plain, a);
    prescribe(q, cut, a);
    const auto r = solve(plain, p, MPI_COMM_SELF), s = solve(cut, q, MPI_COMM_SELF);
    std::map<std::int64_t, V> by_gid;
    for (std::size_t c = 0; c < plain.gid.size(); ++c)
        by_gid[plain.gid[c]] = r.U_cell[c];
    std::vector<V> expected;
    for (const auto gid : cut.gid)
        expected.push_back(by_gid.at(gid));
    field_near(s.U_cell, expected, 0, 1e-10, "unclipped polyhedral/tensor equivalence by gid");
    // The two builders enumerate faces differently; compare geometry-keyed outputs.
    for (std::size_t f = 0; f < cut.faces.size(); ++f) {
        bool found = false;
        for (std::size_t g = 0; g < plain.faces.size(); ++g)
            if (norm(sub(cut.faces[f].centroid, plain.faces[g].centroid)) < 1e-13 &&
                norm(sub(cut.faces[f].normal, plain.faces[g].normal)) < 1e-13) {
                require(norm(sub(s.U_face[f], r.U_face[g])) <= 1e-10 * r.U_mag_max,
                        "uncut face reconstruction equivalence");
                require(s.face_prescribed[f] == r.face_prescribed[g], "uncut flags");
                found = true;
                break;
            }
        require(found, "unclipped face geometric match");
    }
}
void top_selection() {
    auto m = grid(3, 2, 2);
    // Top faces become horizontal embedded faces. A lower upward face must not qualify.
    for (std::size_t i = 0; i < m.faces.size(); ++i)
        if (m.faces[i].side == 5) {
            m.faces[i].side = -1;
            m.is_embedded[i] = 1;
        }
    auto p = problem(m);
    const auto r = solve(m, p, MPI_COMM_SELF);
    check_output(m, p, r);
    require(r.top_samples.size() == 6, "horizontal embedded top faces selected");
    m.z_top += 1;
    p = problem(m);
    const auto empty = solve(m, p, MPI_COMM_SELF);
    check_output(m, p, empty);
    require(empty.top_samples.empty() && empty.warpage_pv == 0,
            "empty top samples give zero warpage");
}
void plane_removal() {
    auto x = axis(4, 8), y = axis(3, 6);
    for (auto& v : x)
        v += 1e6;
    for (auto& v : y)
        v -= 2e6;
    const auto m = grid(x, y, axis(2));
    auto p = problem(m, isotropic(), 0);
    p.support = ElasticProblem::Support::Fixed;
    for (std::size_t f = 0; f < m.faces.size(); ++f) {
        const auto& face = m.faces[f];
        if (face.hi >= 0)
            continue;
        const double qx = (face.centroid[0] - 1e6) / 8;
        const double qy = (face.centroid[1] + 2e6) / 6;
        p.prescribed.push_back(
            {static_cast<std::int64_t>(f), {0, 0, 1e-6 * (qx * qx + 0.4 * qx - 0.2 * qy + 1)}});
    }
    const auto r = solve(m, p, MPI_COMM_SELF);
    check_output(m, p, r);
    // For qx={1,3,5,7}/8, projecting qx^2 off {1,qx,qy}
    // gives residuals {1,-1,-1,1}/16 and peak-to-valley 1/8.
    near(r.warpage_pv, 0.125e-6, 1e-17, 1e-10, "centered scaled least-squares plane removal");
}
void distributed(Suite& suite, int ranks) {
    for (const std::string name : {"replicated graded", "replicated cut", "zero owned rows"})
        for (const std::string pc : {"jacobi", "gamg"})
            for (bool fixed : {false, true})
                suite.run(name + " " + pc + (fixed ? " Fixed" : " Rigid321"), [&] {
                    auto m = name == "replicated cut"
                                 ? clipped(4, 1.4, ranks)
                                 : grid(axis(4, 1, 1.3), axis(3), axis(4, 0.05), ranks);
                    if (name == "zero owned rows")
                        std::fill(m.rank.begin(), m.rank.end(), 0);
                    auto p = problem(m, orthotropic());
                    if (fixed) {
                        p.support = ElasticProblem::Support::Fixed;
                        p.fixed_side = {0};
                    }
                    p.preconditioner = pc;
                    const auto full = solve(m, p, suite.comm);
                    // Complete collectives before local assertions so a rank-specific
                    // output defect reports a failure instead of deadlocking the test.
                    auto root = full;
                    auto broadcast = [&](std::vector<V>& data) {
                        std::uint64_t n = data.size();
                        MPI_Bcast(&n, 1, MPI_UINT64_T, 0, suite.comm);
                        data.resize(static_cast<std::size_t>(n));
                        MPI_Bcast(data.data(), static_cast<int>(n * 3), MPI_DOUBLE, 0, suite.comm);
                    };
                    broadcast(root.U_cell);
                    broadcast(root.U_face);
                    broadcast(root.top_samples);
                    std::uint64_t nf = root.face_prescribed.size();
                    MPI_Bcast(&nf, 1, MPI_UINT64_T, 0, suite.comm);
                    root.face_prescribed.resize(static_cast<std::size_t>(nf));
                    MPI_Bcast(
                        root.face_prescribed.data(), static_cast<int>(nf), MPI_CHAR, 0, suite.comm);
                    MPI_Bcast(&root.warpage_pv, 1, MPI_DOUBLE, 0, suite.comm);
                    MPI_Bcast(&root.U_mag_max, 1, MPI_DOUBLE, 0, suite.comm);
                    MPI_Bcast(&root.residual, 1, MPI_DOUBLE, 0, suite.comm);
                    MPI_Bcast(&root.iterations, 1, MPI_INT, 0, suite.comm);
                    auto serial_mesh = m;
                    std::fill(serial_mesh.rank.begin(), serial_mesh.rank.end(), 0);
                    const auto serial = solve(serial_mesh, p, MPI_COMM_SELF);
                    check_output(m, p, full);
                    // Explicit gid alignment; sparse descending gids interleave owners.
                    std::map<std::int64_t, V> by_gid;
                    for (std::size_t i = 0; i < m.gid.size(); ++i)
                        by_gid[serial_mesh.gid[i]] = serial.U_cell[i];
                    std::vector<V> reference;
                    for (const auto gid : m.gid)
                        reference.push_back(by_gid.at(gid));
                    field_near(
                        full.U_cell, reference, 0, 1e-8, "np2 versus COMM_SELF aligned by gid");
                    compare_results(full, serial, 1e-8);
                    require(full.U_cell == root.U_cell && full.U_face == root.U_face &&
                                full.top_samples == root.top_samples &&
                                full.face_prescribed == root.face_prescribed &&
                                full.warpage_pv == root.warpage_pv &&
                                full.U_mag_max == root.U_mag_max &&
                                full.residual == root.residual &&
                                full.iterations == root.iterations,
                            "identical full outputs and diagnostics on every rank");
                });
}
std::pair<double, double>
physical_residual(const ElasticMesh& m, const ElasticProblem& p, const ElasticResult& r) {
    const auto a = assembly(m, p);
    std::vector<V> residual = a.rhs;
    double rhs2 = 0;
    for (auto& v : residual) {
        rhs2 += dot(v, v);
        v = scale(v, -1);
    }
    for (const auto& b : a.blocks)
        residual[b.row] = add(residual[b.row], mul(b.value, r.U_cell[b.column]));
    double residual2 = 0;
    for (const auto& v : residual)
        residual2 += dot(v, v);
    return {std::sqrt(residual2), std::sqrt(rhs2)};
}
void benchmark(Suite& s) {
    for (const std::string pc : {"jacobi", "gamg"})
        s.run("68x68x6 benchmark " + pc, [&] {
            auto m = grid(axis(68, 0.068), axis(68, 0.068), axis(6, 0.0012));
            auto p = problem(m);
            for (std::size_t c = 0; c < m.volume.size(); ++c) {
                const auto mat = m.centroid[c][2] > 0.0006 ? laminate(0.3) : isotropic();
                p.C[c] = mat.C;
                p.alpha[c] = mat.alpha;
            }
            p.preconditioner = pc;
            const auto begin = std::chrono::steady_clock::now();
            const auto r = solve(m, p, MPI_COMM_SELF);
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
            check_output(m, p, r);
            rusage usage{};
            require(getrusage(RUSAGE_SELF, &usage) == 0, "peak RSS available");
            const auto [true_residual, rhs_norm] = physical_residual(m, p, r);
            require(std::isfinite(true_residual), "finite recomputed physical residual");
            std::cout << "实测 " << pc << " elapsed_s=" << elapsed << " iterations=" << r.iterations
                      << " physical_residual=" << true_residual << " rhs_norm=" << rhs_norm
                      << " reported_residual=" << r.residual << " peak_rss_KiB=" << usage.ru_maxrss
                      << '\n';
        });
}
} // namespace
int main(int argc, char** argv) {
    if (PetscInitialize(&argc, &argv, nullptr, nullptr) != PETSC_SUCCESS)
        return 2;
    KSPInitializePackage();
    PetscLogDefaultBegin();
    Suite s(MPI_COMM_WORLD);
    char mode_buffer[32]{};
    PetscOptionsGetString(
        nullptr, nullptr, "-elastic_test_mode", mode_buffer, sizeof(mode_buffer), nullptr);
    const std::string mode = mode_buffer;
    int ranks = 1;
    MPI_Comm_size(s.comm, &ranks);
    if (mode == "mpi") {
        s.run("np2 launcher",
              [&] { require(ranks == 2, "MPI acceptance requires exactly two ranks"); });
        distributed(s, ranks);
    } else if (mode == "benchmark") {
        benchmark(s);
    } else {
        s.run("clipped fixture exact moments", geometry);
        const std::vector<std::pair<std::string, ElasticMesh>> grids{
            {"uniform", grid(3, 3, 3)},
            {"graded ratio<=2", grid(axis(4, 1, 1.5), axis(3, 0.7, 1.3), axis(3, 0.4, 1.4))},
            {"thin aspect>=50", grid(axis(3, 0.03), axis(3, 0.03), axis(4, 0.0004))},
            {"single layer", grid(3, 2, 1)},
            {"single column", grid(1, 1, 8)},
            {"one cell", grid(1, 1, 1)}};
        for (const auto& [name, m] : grids) {
            s.run(name + " free expansion", [&] { expansion(m); });
            s.run(name + " orthotropic affine", [&] { affine_patch(m); });
            for (bool fixed : {false, true})
                s.run(name + (fixed ? " zero fixed" : " zero rigid"), [&] { zero(m, fixed); });
        }
        s.run("matrix conservation and rigid nullspaces", matrix_invariants);
        for (int i = 0; i < 3; ++i)
            s.run("engineering shear " + std::to_string(i), [&] { shear(i); });
        for (double f : {0.0, 0.3, 1.0})
            s.run("copper adapter fraction " + std::to_string(f), [&] {
                const auto mat = laminate(f);
                if (f == 0 || f == 1) {
                    const auto ref = f == 0 ? isotropic() : isotropic(110e9, 0.34, 17e-6);
                    for (int j = 0; j < 36; ++j)
                        near(mat.C[j], ref.C[j], 1e-5, 1e-13, "laminate isotropic endpoint");
                    for (int j = 0; j < 3; ++j)
                        near(mat.alpha[j], ref.alpha[j], 1e-20, 1e-13, "laminate alpha endpoint");
                }
                affine_patch(grid(3, 2, 3),
                             {0.001, 0.002, 0.003, -0.001, 0.003, 0.002, 0.001, -0.002, 0.004},
                             mat);
                auto m = grid(3, 2, 3);
                auto p = problem(m, mat);
                M3 a{};
                for (int j = 0; j < 3; ++j)
                    a[3 * j + j] = mat.alpha[j] * 80;
                prescribe(p, m, a);
                const auto r = solve(m, p, MPI_COMM_SELF);
                traction(m, p, r, 1e-10);
            });
        validation(s);
        more_validation(s);
        s.run("Fixed expansion and duplicate prescribed", supports);
        s.run("nonconvergence reports iterations and residual", failed_ksp);
        s.run("jacobi versus gamg", preconditioners);
        s.run("cut oblique expansion", [] { expansion(clipped(4), 1e-8, true); });
        s.run("cut prescribed cantilever", cut_support);
        s.run("uncut polyhedral equivalence", uncut_equivalence);
        s.run("embedded top and empty sample selection", top_selection);
        s.run("shifted coordinates quadratic plane removal", plane_removal);
        s.run("free gamg expansion", [] {
            const auto m = grid(3, 3, 3);
            auto p = problem(m);
            p.preconditioner = "gamg";
            const auto r = solve(m, p, MPI_COMM_SELF);
            check_output(m, p, r);
            const double strain = 80e-5;
            const M3 a{strain, 0, 0, 0, strain, 0, 0, 0, strain};
            field_near(r.U_cell,
                       affine(m.centroid, a, scale(center(m), -strain)),
                       1e-10 * length(m) * strain,
                       0,
                       "GAMG rigid near-nullspace expansion");
        });
    }
    const int result = s.finish();
    PetscFinalize();
    return result;
}
