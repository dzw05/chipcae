#include "chipcae/material/activation.hpp"
#include "chipcae/material/cte_convert.hpp"
#include "chipcae/material/material_params.hpp"
#include "chipcae/material/schema.hpp"
#include "chipcae/material/state_io.hpp"
#include "single_point.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>
using namespace chipcae::material;
constexpr unsigned seed = 20261116;
std::mt19937_64 rng(seed);
std::string id;
Real measured = 0, tolerance = 0;
PointInput last_input;
void require(bool ok, const char* note) {
    if (!ok) {
        std::fprintf(stderr,
                     "FAIL %s seed=%u: %s T=(%.17g,%.17g) strain=",
                     id.c_str(),
                     seed,
                     note,
                     last_input.T_n,
                     last_input.T_np1);
        for (auto v : last_input.strain_np1)
            std::fprintf(stderr, " %.17g", v);
        std::fprintf(stderr, " measured=%.17g tolerance=%.17g\n", measured, tolerance);
        std::exit(1);
    }
}
void error(Real e, Real tol, const char* note) {
    if (tolerance == 0 || (tol > 0 && e / tol > measured / tolerance)) {
        measured = e;
        tolerance = tol;
    }
    if (!(std::isfinite(e) && e <= tol))
        std::fprintf(stderr, "comparison %s: error=%.17g limit=%.17g\n", note, e, tol);
    require(std::isfinite(e) && e <= tol, note);
}
void close(Real a, Real b, Real tol = 1e-12) {
    error(
        std::abs(a - b) / std::max({1e-30, std::abs(a), std::abs(b)}), tol, "analytic comparison");
}
template <class A, class B>
Real relative(const A& a, const B& b) {
    Real d = 0, n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        d += (a[i] - b[i]) * (a[i] - b[i]);
        n += b[i] * b[i];
    }
    return std::sqrt(d / std::max(n, 1e-60));
}
Real random(Real a, Real b) {
    return std::uniform_real_distribution<Real>(a, b)(rng);
}
ThermoElastic elastic(Real E = 2e9, Real nu = .25, ThermalStrain th = {}) {
    return ThermoElastic(
        ElasticSymmetry::Isotropic, TemperatureTable({200, 500}, {{E, E}, {nu, nu}}), th);
}
PronyViscoelastic prony(VolumetricMode mode = VolumetricMode::ElasticBulk,
                        ShiftFunction shift = {},
                        ThermalStrain thermal = {}) {
    PronyParams p;
    p.mode = mode;
    p.infinity = 2e9;
    p.terms = {{3e9, .7}, {1e9, 3.}};
    p.poisson = .25;
    return PronyViscoelastic(p, TemperatureTable({200, 500}, {{4e9, 4e9}}), shift, thermal);
}
template <class M>
struct Point {
    M m;
    std::vector<Real> old, next;
    PointOutput out;
    PointInput in;
    StepContext ctx;
    Point(M model) : m(model), old(m.layout().n_reals), next(old.size()) {
        m.init_state({old.data(), 1});
        in.T_n = in.T_np1 = 300;
    }
    void eval() {
        last_input = in;
        require(m.update(ctx, in, {old.data(), 1}, {next.data(), 1}, out) == StatusCode::Ok,
                "update status");
    }
    void step() {
        eval();
        old = next;
        in.strain_n = in.strain_np1;
        in.T_n = in.T_np1;
    }
};
using Shrunk = WithEigenstrains<ThermoElastic, EquivalentCureShrinkage>;
EquivalentCureShrinkage source() {
    return {ShrinkageSchedule(.006, 2, 0, 10)};
}
template <class M>
void tangent(M model) {
    Point p(model);
    for (auto kin : {Kinematics::SmallStrain, Kinematics::TotalLagrangian})
        for (int sample = 0; sample < 100; ++sample) {
            p.ctx.kin = kin;
            p.ctx.dt = random(0, 2);
            p.ctx.clock = {2, 0, random(0, 10), 0, 0};
            p.in.T_n = random(290, 310);
            p.in.T_np1 = random(320, 340);
            for (auto& v : p.in.strain_np1)
                v = random(-.002, .002);
            for (auto& v : p.in.strain_n)
                v = random(-.002, .002);
            for (auto& v : p.old)
                v = random(-1e-5, 1e-5);
            // Valid randomized activation record; remaining state is arbitrary finite history.
            if constexpr (requires { M::activation_prefix; }) {
                p.old[0] = 1;
                p.old[1] = 300;
                p.old[8] = 1;
            }
            p.eval();
            auto C = p.out.tangent;
            auto eps = p.in.strain_np1;
            Real h = 1e-6 * std::max(mandel_norm(eps), 1e-4);
            Mat6 fd{};
            for (int j = 0; j < 6; ++j) {
                p.in.strain_np1 = eps;
                p.in.strain_np1[j] += h;
                p.eval();
                auto plus = p.out.stress;
                p.in.strain_np1 = eps;
                p.in.strain_np1[j] -= h;
                p.eval();
                for (int i = 0; i < 6; ++i)
                    fd[6 * i + j] = (plus[i] - p.out.stress[i]) / (2 * h);
            }
            error(relative(fd, C), 1e-6, "FD tangent");
        }
}
void t1() {
    tangent(elastic());
    for (auto mode : {VolumetricMode::ElasticBulk, VolumetricMode::ConstantPoissonFromE}) {
        tangent(prony(mode));
        tangent(WithActivation{WithEigenstrains{prony(mode), source()}});
    }
    tangent(WithActivation{Shrunk{elastic(), source()}});
}
template <class M>
void spd(M model) {
    Point p(model);
    p.ctx.dt = .5;
    p.eval();
    auto C = p.out.tangent;
    auto Ct = mat6_transpose(C);
    error(relative(C, Ct), 1e-13, "symmetry");
    // Jacobi eigenvalues, independent of the constitutive Cholesky.
    for (int sweep = 0; sweep < 100; ++sweep)
        for (int i = 0; i < 6; ++i)
            for (int j = i + 1; j < 6; ++j) {
                if (std::abs(C[6 * i + j]) < 1e-12)
                    continue;
                Real angle = .5 * std::atan2(2 * C[6 * i + j], C[6 * j + j] - C[6 * i + i]);
                Real c = std::cos(angle), s = std::sin(angle);
                for (int k = 0; k < 6; ++k) {
                    Real a = C[6 * k + i], b = C[6 * k + j];
                    C[6 * k + i] = c * a - s * b;
                    C[6 * k + j] = s * a + c * b;
                }
                for (int k = 0; k < 6; ++k) {
                    Real a = C[6 * i + k], b = C[6 * j + k];
                    C[6 * i + k] = c * a - s * b;
                    C[6 * j + k] = s * a + c * b;
                }
            }
    for (int i = 0; i < 6; ++i)
        require(C[6 * i + i] > 0, "positive minimum eigenvalue");
}
void t2() {
    spd(elastic());
    spd(prony());
    spd(prony(VolumetricMode::ConstantPoissonFromE));
}
Mat3 multiply(const Mat3& a, const Mat3& b) {
    Mat3 c{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k)
                c[3 * i + j] += a[3 * i + k] * b[3 * k + j];
    return c;
}
Mat3 transpose(const Mat3& a) {
    return {a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8]};
}
Mat3 rotation() {
    Real a = random(-3.14, 3.14), b = random(-3.14, 3.14);
    return multiply(Mat3{std::cos(a), -std::sin(a), 0, std::sin(a), std::cos(a), 0, 0, 0, 1},
                    Mat3{1, 0, 0, 0, std::cos(b), -std::sin(b), 0, std::sin(b), std::cos(b)});
}
Vec6 green(const Mat3& F) {
    auto E = multiply(transpose(F), F);
    for (int i = 0; i < 9; ++i)
        E[i] = (E[i] - (i % 4 == 0 ? 1 : 0)) * .5;
    return {E[0], E[4], E[8], kMandelSqrt2 * E[5], kMandelSqrt2 * E[2], kMandelSqrt2 * E[1]};
}
Mat3 tensor(const Vec6& s) {
    return {s[0],
            s[5] / kMandelSqrt2,
            s[4] / kMandelSqrt2,
            s[5] / kMandelSqrt2,
            s[1],
            s[3] / kMandelSqrt2,
            s[4] / kMandelSqrt2,
            s[3] / kMandelSqrt2,
            s[2]};
}
Mat3 push(const Mat3& F, const Vec6& S) {
    Real J = F[0] * (F[4] * F[8] - F[5] * F[7]) - F[1] * (F[3] * F[8] - F[5] * F[6]) +
             F[2] * (F[3] * F[7] - F[4] * F[6]);
    auto c = multiply(multiply(F, tensor(S)), transpose(F));
    for (auto& v : c)
        v /= J;
    return c;
}
template <class M>
void objectivity(M model) {
    Point p(model);
    p.ctx.kin = Kinematics::TotalLagrangian;
    p.ctx.dt = .7;
    p.ctx.clock = {2, 0, 3, 0, 0};
    for (int i = 0; i < 100; ++i) {
        Mat3 F{1.2, .05, .02, .01, .9, .07, .04, .03, 1.1};
        auto Q = rotation(), QF = multiply(Q, F);
        p.in.strain_np1 = green(F);
        p.eval();
        auto out = p.out;
        auto state = p.next;
        p.in.strain_np1 = green(QF);
        p.eval();
        error(relative(p.out.stress, out.stress), 1e-12, "PK2 invariance");
        if (!state.empty())
            error(relative(p.next, state), 1e-12, "internal invariance");
        auto c = push(F, out.stress);
        auto rotated = multiply(multiply(Q, c), transpose(Q));
        error(relative(push(QF, p.out.stress), rotated), 1e-12, "Cauchy push forward");
    }
}
void t4() {
    objectivity(elastic());
    objectivity(prony());
    objectivity(Shrunk{elastic(), source()});
    objectivity(WithEigenstrains{prony(), source()});
}
void t5() {
    for (auto sym : {ElasticSymmetry::Isotropic,
                     ElasticSymmetry::TransverselyIsotropic,
                     ElasticSymmetry::Orthotropic}) {
        auto m = sym == ElasticSymmetry::Isotropic
                     ? elastic()
                     : ThermoElastic(
                           sym,
                           sym == ElasticSymmetry::Orthotropic
                               ? TemperatureTable(
                                     {300},
                                     {{10e9}, {8e9}, {5e9}, {.2}, {.1}, {.15}, {3e9}, {2e9}, {1e9}})
                               : TemperatureTable({300}, {{10e9}, {5e9}, {.2}, {.1}, {2e9}}));
        Point p(m);
        for (int n = 0; n < 100; ++n) {
            p.in.R = nullptr;
            for (auto& v : p.in.strain_np1)
                v = random(-.01, .01);
            p.eval();
            auto base = p.out;
            auto eps = p.in.strain_np1;
            auto R = rotation();
            auto Qt = mat6_transpose(mandel_rotation_matrix(R));
            p.in.R = &R;
            p.in.strain_np1 = mat6_vec(Qt, eps);
            p.eval();
            error(relative(p.out.stress, mat6_vec(Qt, base.stress)),
                  1e-12,
                  "anisotropic joint rotation");
            if (sym == ElasticSymmetry::Isotropic) {
                p.in.strain_np1 = eps;
                p.eval();
                error(relative(p.out.stress, base.stress), 1e-12, "isotropic R independence");
            }
        }
    }
}
Real G(Real t) {
    return 2e9 + 3e9 * std::exp(-t / .7) + 1e9 * std::exp(-t / 3);
}
void t6() {
    for (int n : {1, 10, 17}) {
        Point p(prony());
        p.in.strain_np1[5] = .01;
        p.step();
        Real time = 0;
        for (int i = 0; i < n; ++i) {
            p.ctx.dt = (i + 1.) / (n * (n + 1.) / 2) * 4;
            time += p.ctx.dt;
            p.step();
            close(p.out.stress[5], 2 * G(time) * .01);
        }
    }
}
void t7() {
    Point p(prony());
    Real t = 0;
    for (int i = 0; i < 19; ++i) {
        p.ctx.dt = .01 + (i + 1) * .047;
        t += p.ctx.dt;
        p.in.strain_np1[5] = .003 * t;
        p.step();
        Real v = 2 * .003 *
                 (2e9 * t + 3e9 * .7 * (-std::expm1(-t / .7)) + 1e9 * 3 * (-std::expm1(-t / 3)));
        close(p.out.stress[5], v);
    }
}
MaterialParams params(bool visco = false, bool shrink = false, bool activation = false) {
    MaterialParams p;
    p.kind = visco ? ModelKind::PronyViscoelastic : ModelKind::ThermoElastic;
    p.elastic_table = TemperatureTable({200, 500}, {{2e9, 2e9}, {.25, .25}});
    p.thermal.T_sf = 300;
    p.cte_table = TemperatureTable({200, 500}, {{1e-5, 1e-5}});
    p.prony.infinity = 2e9;
    p.prony.terms = {{3e9, .7}, {1e9, 3}};
    p.bulk_table = TemperatureTable({200, 500}, {{4e9, 4e9}});
    p.shift.T_ref = 300;
    p.shift.C2 = 100;
    p.has_shrinkage = shrink;
    p.has_activation = activation;
    p.shrinkage = source().schedule;
    return p;
}
Real creep(int n) {
    auto m = make_material<Kokkos::HostSpace>(params(true));
    driver::SinglePointDriver d(*m);
    Vec6 target{1e6, 0, 0, 0, 0, 0};
    require(d.stress_control(target, 300, 0).status.code == StatusCode::Ok, "creep instantaneous");
    driver::DriverResult r;
    for (int i = 0; i < n; ++i) {
        r = d.stress_control(target, 300, 2. / n);
        require(r.status.code == StatusCode::Ok, "Newton convergence");
    }
    return r.strain[0];
}
void t8() {
    Real ref = creep(8192);
    std::vector<Real> errors;
    for (int n : {16, 32, 64, 128})
        errors.push_back(std::abs(creep(n) - ref));
    for (int i = 1; i < 4; ++i) {
        Real order = std::log2(errors[i - 1] / errors[i]);
        std::printf("order=%.17g\n", order);
        error(std::abs(order - 2), .2, "creep order 1.8..2.2");
    }
}
void t9() {
    ShiftParams sp;
    sp.type = ShiftType::Arrhenius;
    sp.T_ref = 300;
    sp.E_a = 30000;
    ShiftFunction shift(sp);
    Real loga;
    shift.evaluate(330, loga);
    Real a = std::pow(10., loga);
    Point p(prony(VolumetricMode::ElasticBulk, shift)),
        q(prony(VolumetricMode::ElasticBulk, shift));
    p.in.T_n = p.in.T_np1 = 330;
    p.in.strain_np1[5] = q.in.strain_np1[5] = .01;
    p.step();
    q.step();
    for (int i = 0; i < 50; ++i) {
        p.ctx.dt = .03;
        q.ctx.dt = .03 / a;
        p.step();
        q.step();
        error(relative(p.out.stress, q.out.stress), 1e-12, "time temperature superposition");
    }
}
void t10() {
    auto m = make_material<Kokkos::HostSpace>(params(true, true));
    driver::SinglePointDriver d(*m);
    Vec6 strain{};
    for (int i = 0; i < 5; ++i) {
        strain[5] = i * .001;
        d.strain_control(strain, 300, .1);
    }
    // Include source state in checkpoint, even when the process clock is outside cure.
    auto& store = d.store();
    auto tr = store.trial();
    Kokkos::deep_copy(tr, store.committed());
    tr(store.layout().n_reals - 1, 0) = .003;
    store.commit();
    BlobStateWriter saved;
    store.save(saved);
    auto replay = [&] {
        for (int i = 0; i < 7; ++i) {
            strain[5] = .01 * (i + 1);
            d.strain_control(strain, 300, .1);
        }
        BlobStateWriter w;
        store.save(w);
        return w.bytes();
    };
    auto first = replay();
    BlobStateReader r(saved.bytes());
    store.restore(r);
    auto second = replay();
    require(first.size() == second.size() &&
                std::memcmp(first.data(), second.data(), first.size()) == 0,
            "bitwise checkpoint replay including s_applied");
    BlobStateWriter before;
    store.save(before);
    Kokkos::deep_copy(store.trial(), 123.);
    store.discard();
    BlobStateWriter after;
    store.save(after);
    require(before.bytes() == after.bytes(), "discard bitwise unchanged");
    QuadratureStateStore<Kokkos::HostSpace> wrong(store.layout(), 1, 99);
    bool rejected = false;
    try {
        BlobStateReader rr(saved.bytes());
        wrong.restore(rr);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "parameter mismatch rejected");
}
void t11() {
    CteTable tan{{250, 300, 350}, {1e-5, 2e-5, 3e-5}, 250};
    ThermalStrainParams tp;
    tp.T_sf = 275;
    ThermalStrain th(
        tp,
        TemperatureTable(
            tan.temperatures, {tan.alpha}, Interpolation::Linear, Extrapolation::Clamp));
    for (Real T : {250., 275., 300., 325., 350.}) {
        Real secT, sec0;
        tangent_to_secant(tan, T, secT);
        tangent_to_secant(tan, tp.T_sf, sec0);
        Vec6 eps;
        th.evaluate(T, eps);
        Real expected = secT * (T - 250) - sec0 * (tp.T_sf - 250);
        error(std::abs(eps[0] - expected), 1e-12, "CTE exact conversion");
        Point p(elastic(2e9, .25, th));
        p.in.T_np1 = T;
        p.in.strain_np1 = eps;
        p.eval();
        error(mandel_norm(p.out.stress), 1e-10 * 2e9 * 1e-5 * 100, "free expansion");
    }
    std::vector<Real> secant_nodes;
    tangent_to_secant(tan, secant_nodes);
    auto secparams = tp;
    secparams.cte_type = CteType::Secant;
    secparams.T_ref = 250;
    ThermalStrain secant(
        secparams,
        TemperatureTable(
            tan.temperatures, {secant_nodes}, Interpolation::Linear, Extrapolation::Clamp));
    for (int i = 0; i <= 100; ++i) {
        Vec6 a, b;
        Real T = 250 + i;
        th.evaluate(T, a);
        secant.evaluate(T, b);
        error(relative(a, b), 1e-12, "tangent and secant kernel equivalence");
    }
    Vec6 zero;
    th.evaluate(tp.T_sf, zero);
    require(mandel_norm(zero) == 0, "thermal zero");
}
void t12() {
    Point p(prony());
    p.in.strain_np1[5] = .01;
    p.eval();
    close(p.out.tangent[35], 12e9, 1e-13);
    p.ctx.dt = 1e30;
    p.eval();
    close(p.out.tangent[35], 4e9, 1e-13);
    for (auto v : p.out.stress)
        require(std::isfinite(v), "large x finite");
    auto g = [](Real x) {
        return x < 1e-4 ? 1 - x / 2 + x * x / 6 - x * x * x / 24 : -std::expm1(-x) / x;
    };
    close(g(1e-4 * (1 - 1e-10)), g(1e-4 * (1 + 1e-10)), 1e-13);
}
void t13() {
    Point p(prony());
    p.ctx.need_dissipation = true;
    for (int n = 0; n < 1000; ++n) {
        p.ctx.dt = random(0, 2);
        p.in.T_np1 = random(280, 360);
        Vec6 de;
        for (int i = 0; i < 6; ++i) {
            p.in.strain_np1[i] = random(-.01, .01);
            de[i] = p.in.strain_np1[i] - p.in.strain_n[i];
        }
        p.step();
        Real scale = mandel_norm(p.out.stress) * mandel_norm(de);
        error(std::max(Real{0}, -p.out.dissipation) / std::max(scale, Real{1e-300}),
              1e-14,
              "nonnegative dissipation");
    }
}
struct Batch {
    Soa<Real, Kokkos::HostSpace> e, en, s, sn;
    Vec1<Real, Kokkos::HostSpace> T;
    BatchInput<Kokkos::HostSpace> in;
    BatchOutput<Kokkos::HostSpace> out;
    Batch(const IMaterial<Kokkos::HostSpace>& m, int n)
        : e("e", 6, n), en("en", 6, n), s("s", m.layout().n_reals, n),
          sn("sn", m.layout().n_reals, n), T("T", n) {
        Kokkos::deep_copy(T, 300.);
        m.init_state(s);
        in.strain_n = en;
        in.strain_np1 = e;
        in.T_n = T;
        in.T_np1 = T;
        out.stress = Soa<Real, Kokkos::HostSpace>("stress", 6, n);
        out.tangent = Soa<Real, Kokkos::HostSpace>("C", 36, n);
        out.thermal_strain = Soa<Real, Kokkos::HostSpace>("th", 6, n);
        out.chemical_strain = Soa<Real, Kokkos::HostSpace>("ch", 6, n);
        out.status = Vec1<Status, Kokkos::HostSpace>("status", n);
    }
};
void t15() {
    for (bool v : {false, true})
        for (bool shrink : {false, true})
            for (bool activation : {false, true}) {
                auto par = params(v, shrink, activation);
                auto m = make_material<Kokkos::HostSpace>(par);
                constexpr int N = 37;
                Batch b(*m, N);
                StepContext ctx;
                ctx.dt = .2;
                ctx.clock = {2, 0, 3, 0, 0};
                for (int n = 0; n < N; ++n)
                    for (int j = 0; j < 6; ++j)
                        b.e(j, n) = random(-.01, .01);
                m->update_batch(ctx, b.in, b.s, b.sn, b.out);
                Kokkos::fence();
                auto compare = [&](auto model) {
                    for (int n = 0; n < N; ++n) {
                        Point q(model);
                        q.ctx = ctx;
                        for (int j = 0; j < 6; ++j)
                            q.in.strain_np1[j] = b.e(j, n);
                        q.eval();
                        for (int j = 0; j < 6; ++j)
                            require(q.out.stress[j] == b.out.stress(j, n),
                                    "batch vs point stress bitwise");
                        for (int j = 0; j < 36; ++j)
                            require(q.out.tangent[j] == b.out.tangent(j, n),
                                    "batch vs point tangent bitwise");
                        for (unsigned j = 0; j < m->layout().n_reals; ++j)
                            require(q.next[j] == b.sn(j, n), "batch vs point state bitwise");
                        require(q.out.status.code == b.out.status(n).code, "batch vs point status");
                    }
                };
                auto combine = [&](auto base) {
                    if (shrink) {
                        auto eig = WithEigenstrains{base, source()};
                        if (activation)
                            compare(WithActivation{eig});
                        else
                            compare(WithActivation<decltype(eig), false>{eig});
                    } else {
                        if (activation)
                            compare(WithActivation{base});
                        else
                            compare(WithActivation<decltype(base), false>{base});
                    }
                };
                auto thermal = ThermalStrain{par.thermal, par.cte_table};
                if (v)
                    combine(PronyViscoelastic{
                        par.prony, par.bulk_table, ShiftFunction{par.shift}, thermal});
                else
                    combine(ThermoElastic{par.elastic_symmetry, par.elastic_table, thermal});
                // Persist complete result for the external 1-thread/N-thread bitwise comparison.
                if (const char* path = std::getenv("K3_BATCH_DUMP")) {
                    std::ofstream f(path, std::ios::binary | std::ios::app);
                    f.write(reinterpret_cast<const char*>(b.out.stress.data()),
                            sizeof(Real) * 6 * N);
                    f.write(reinterpret_cast<const char*>(b.sn.data()),
                            sizeof(Real) * m->layout().n_reals * N);
                }
            }
}
void t16(const char* directory) {
    require(validate_material_file(std::string(directory) + "/../valid_elastic.json").ok,
            "valid elastic fixture");
    require(validate_material_file(std::string(directory) + "/../valid_prony.json").ok,
            "valid Prony fixture");
    std::ifstream index(std::string(directory) + "/index.txt");
    require(bool(index), "fixture index");
    std::string path;
    int n = 0;
    while (std::getline(index, path)) {
        auto r = validate_material_file(std::string(directory) + "/" + path,
                                        path.starts_with("customer_delivery"));
        require(!r.ok && !r.issues.empty(), path.c_str());
        for (auto& issue : r.issues)
            require(!issue.field_path.empty() && !issue.message.empty(), "readable field error");
        ++n;
    }
    require(n >= 30, "semantic rule fixtures");
    // A load history is checked at update, not when validating an otherwise valid card.
    for (bool fitted : {false, true}) {
        auto card = std::string(directory) +
                    (fitted ? "/../load_fit_range.json" : "/../load_table_range.json");
        std::ifstream f(card);
        std::stringstream text;
        text << f.rdbuf();
        auto par = parse_material_json(text.str());
        auto material = make_material<Kokkos::HostSpace>(par);
        driver::SinglePointDriver d(*material);
        require(d.strain_control({}, 450, 1).status.code == StatusCode::OutOfRange,
                "load temperature rejected by table/fit range");
        if (fitted) {
            par.fit_temperature.clamp = true;
            auto clamped = make_material<Kokkos::HostSpace>(par);
            driver::SinglePointDriver c(*clamped);
            require(c.strain_control({}, 450, 1).status.code == StatusCode::Ok &&
                        clamped->clamped_warning(),
                    "fit clamp and host warning");
        }
    }
}
void t17() {
    StateField fields[] = {{"linear", 0, 2, true}, {"diagnostic", 2, 1, false}};
    StateLayout l{3, fields, 99};
    QuadratureStateStore<Kokkos::HostSpace> old(l, 8, 0, "same"), dest(l, 5, 0, "same");
    std::vector<Real> xyz;
    for (int i = 0; i < 8; ++i) {
        Real x = i & 1, y = (i >> 1) & 1, z = (i >> 2) & 1;
        xyz.insert(xyz.end(), {x, y, z});
        old.trial()(0, i) = 2 + 3 * x - 4 * y + 5 * z;
        old.trial()(1, i) = -3 + x + 2 * y - z;
        old.trial()(2, i) = 99;
    }
    old.commit();
    std::vector<unsigned> indices{0, 1, 2, 3, 4, 5, 6, 7};
    std::vector<PointMapEntry> entries;
    for (int i = 0; i < 5; ++i)
        entries.push_back({unsigned(i), {random(0, 1), random(0, 1), random(0, 1)}, indices, xyz});
    dest.transfer_from(old, {entries});
    for (auto& e : entries) {
        auto [x, y, z] = e.new_coordinate;
        close(dest.committed()(0, e.new_point), 2 + 3 * x - 4 * y + 5 * z);
        close(dest.committed()(1, e.new_point), -3 + x + 2 * y - z);
        require(dest.committed()(2, e.new_point) == 0, "nontransferable field frozen");
    }
    bool rejected = false;
    try {
        QuadratureStateStore<Kokkos::HostSpace> wrong(l, 5, 0, "different");
        wrong.transfer_from(old, {entries});
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "same material id required");
    auto pa = params();
    pa.id = "material_a";
    auto pb = pa;
    pb.id = "material_b";
    auto ma = make_material<Kokkos::HostSpace>(pa), mb = make_material<Kokkos::HostSpace>(pb);
    driver::SinglePointDriver da(*ma), db(*mb);
    rejected = false;
    try {
        db.store().transfer_from(da.store(), {});
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "driver preserves card identity independently of ModelKind");
}
void t18() {
    PronyParams a;
    a.infinity = 2e9;
    a.terms = {{3e9, .7}, {1e9, 3}};
    a.poisson = .25;
    auto b = a;
    b.mode = VolumetricMode::ConstantPoissonFromE;
    auto shear = a;
    shear.infinity /= 2.5;
    for (auto& t : shear.terms)
        t.G /= 2.5;
    Real K = 6e9 / 1.5;
    Point p(PronyViscoelastic(shear, TemperatureTable({300}, {{K}}))),
        q(PronyViscoelastic(b, TemperatureTable({300}, {{K}})));
    for (int i = 0; i < 100; ++i) {
        p.ctx.dt = q.ctx.dt = random(0, 1);
        p.in.strain_np1[5] = q.in.strain_np1[5] = random(-.01, .01);
        p.step();
        q.step();
        error(relative(p.out.stress, q.out.stress), 1e-12, "volume modes shear match");
    }
    p = Point(p.m);
    q = Point(q.m);
    for (int j = 0; j < 3; ++j)
        p.in.strain_np1[j] = q.in.strain_np1[j] = .001;
    p.step();
    q.step();
    Real t = 0;
    for (int i = 0; i < 40; ++i) {
        p.ctx.dt = q.ctx.dt = .05;
        t += .05;
        p.step();
        q.step();
        close(p.out.stress[0], K * .003);
        close(q.out.stress[0], G(t) / 1.5 * .003);
    }
    require(!q.m.approximation_notes().empty(), "approximation metadata");
}
ShiftParams splice(Real ref = 320) {
    ShiftParams sp;
    sp.type = ShiftType::PiecewiseWlfArrhenius;
    sp.T_ref = ref;
    sp.T_g = 300;
    sp.C1 = 8;
    sp.C2 = 100;
    sp.E_a = 30000;
    return sp;
}
void t19() {
    for (Real ref : {280., 320.}) {
        auto sp = splice(ref);
        ShiftFunction f(sp);
        Real L, Lm, Lp, Lref;
        f.evaluate(300, L);
        f.evaluate(300 - 1e-6, Lm);
        f.evaluate(300 + 1e-6, Lp);
        f.evaluate(ref, Lref);
        error(std::abs(Lref), 1e-14, "reference normalization");
        Real w = -sp.C1 * (300 - ref) / (sp.C2 + 300 - ref);
        Real a = w + sp.E_a / (kGasConstant * std::log(10.)) * (1. / 300 - 1. / 300);
        error(std::abs(a - w), 1e-13, "splice branches at Tg");
        Real slope = std::max(sp.C1 * sp.C2 / std::pow(sp.C2 + 300 - ref, 2),
                              sp.E_a / (kGasConstant * std::log(10.) * 300 * 300));
        error(std::abs(Lp - Lm), 2e-6 * slope * (1 + 1e-7), "one-sided continuity");
    }
}
// E1 antiderivatives from the independently supplied formula reference.
Real primitive_A(Real T, Real B) {
    return T * std::exp(-B / T) + B * std::expint(-B / T);
}
Real primitive_W(Real T, Real c, Real B) {
    Real u = T + c;
    return u * std::exp(-B / u) + B * std::expint(-B / u);
}
Real analytic_xi(Real a, Real b, Real dt, const ShiftParams& p) {
    if (a > b)
        std::swap(a, b);
    Real tg = p.T_g;
    Real c = p.C2 - p.T_ref;
    Real B = p.C1 * p.C2 * std::log(10.);
    Real LWg = -p.C1 * (tg - p.T_ref) / (p.C2 + tg - p.T_ref);
    Real EA = p.E_a / kGasConstant;
    Real rawref = p.T_ref >= tg ? -p.C1 * (p.T_ref - p.T_ref) / (p.C2)
                                : LWg + EA / std::log(10.) * (1 / p.T_ref - 1 / tg);
    Real sum = 0;
    if (a < tg) {
        Real hi = std::min(b, tg);
        sum += std::pow(10., rawref - LWg) * std::exp(EA / tg) *
               (primitive_A(hi, EA) - primitive_A(a, EA));
    }
    if (b > tg) {
        Real lo = std::max(a, tg);
        sum += std::pow(10., rawref + p.C1) * (primitive_W(b, c, B) - primitive_W(lo, c, B));
    }
    return dt / (b - a) * sum;
}
void t20() {
    for (Real ref : {280., 320.}) {
        auto sp = splice(ref);
        ShiftFunction f(sp);
        for (auto pair : std::vector<std::pair<Real, Real>>{
                 {340, 270}, {270, 340}, {300, 270}, {340, 300}, {270, 300}, {300, 340}}) {
            Real xi;
            require(f.reduced_time_increment(pair.first, pair.second, 2, xi) == StatusCode::Ok,
                    "cross Tg integration");
            close(xi, analytic_xi(pair.first, pair.second, 2, sp), 1e-10);
        }
    }
}
void t21() {
    auto m = Shrunk{elastic(), source()};
    Point p(m);
    p.ctx.clock = {2, 0, 10, 0, 0};
    p.eval();
    Real K = 2e9 / (3 * (1 - 2 * .25));
    for (int j = 0; j < 3; ++j)
        close(p.out.stress[j], K * .006);
    require(p.next.back() == .006, "s_applied");
    // Free shrinkage through the batch stress-control driver.
    auto factory = make_material<Kokkos::HostSpace>(params(false, true));
    Batch b(*factory, 1);
    StepContext ctx;
    ctx.clock = {2, 0, 10, 0, 0};
    factory->update_batch(ctx, b.in, b.s, b.sn, b.out);
    Kokkos::fence();
    driver::SinglePointDriver d(
        *factory); // driver exposes the process clock for process-bound control.
    d.context().clock = ctx.clock;
    auto r = d.stress_control({}, 300, 0, 30, 1e-7);
    require(r.status.code == StatusCode::Ok, "free shrink Newton");
    for (int j = 0; j < 3; ++j)
        close(r.strain[j], -.002);
    error(mandel_norm(r.stress) / (K * .006), 1e-12, "free shrink zero stress");
    for (bool v : {false, true}) {
        auto run = [&](int n) {
            Point q(WithEigenstrains{prony(), source()});
            Point e(m);
            for (int i = 0; i < n; ++i) {
                q.ctx.dt = e.ctx.dt = 10. / n;
                q.ctx.clock = e.ctx.clock = {2, 10. * i / n, 10. * (i + 1) / n, 0, 0};
                if (v)
                    q.step();
                else
                    e.step();
            }
            return v ? q.out.stress : e.out.stress;
        };
        error(relative(run(1), run(50)), 1e-12, "one vs fifty shrinkage steps");
    }
    // Uniaxial constraint: eps11=0, sigma22=sigma33=0 => sigma11=E*s/3.
    p.in.strain_np1 = {0, -.0025, -.0025, 0, 0, 0};
    p.eval();
    close(p.out.stress[0], 2e9 * .006 / 3);
    error(std::abs(p.out.stress[1]) / (K * .006), 1e-12, "uniaxial lateral free");
    Real ts[] = {0, .25, 1}, fs[] = {0, .1, 1};
    ShrinkageSchedule table(.006, 2, 0, 10, ts, fs);
    close(table.shrinkage({2, 0, 2.5, 0, 0}, true), .0006);
    p.ctx.clock = {1, 0, 10, 0, 0};
    p.eval();
    require(p.next.back() == 0, "before cure");
    p.ctx.clock = {2, 0, 10, 0, 0};
    p.step();
    p.ctx.clock = {3, 0, 5, 0, 0};
    p.eval();
    require(p.next.back() == .006, "after cure hold");
}
void t22() {
    ThermoElastic m(
        ElasticSymmetry::Isotropic,
        TemperatureTable({299, 300, 301, 302}, {{1e9, 1e9, 1e10, 1e10}, {.25, .25, .25, .25}}));
    std::vector<Real> ds, dc;
    for (Real h : {.1, .05, .025}) {
        Point p(m);
        p.in.strain_np1 = {.001, -.002, .003, .004, .005, .006};
        p.in.T_np1 = 299.5;
        p.eval();
        auto prev = p.out;
        Real maxs = 0, maxc = 0;
        for (int i = 1; i <= int(std::round(2 / h)); ++i) {
            p.in.T_np1 = 299.5 + i * h;
            p.eval();
            Real sn = 0, cn = 0;
            for (int j = 0; j < 6; ++j)
                sn += std::pow(p.out.stress[j] - prev.stress[j], 2);
            for (int j = 0; j < 36; ++j)
                cn += std::pow(p.out.tangent[j] - prev.tangent[j], 2);
            maxs = std::max(maxs, std::sqrt(sn));
            maxc = std::max(maxc, std::sqrt(cn));
            prev = p.out;
        }
        Point unit(elastic(9e9));
        unit.in.strain_np1 = p.in.strain_np1;
        unit.eval();
        Real Ls = mandel_norm(unit.out.stress), Lc = 0;
        for (auto v : unit.out.tangent)
            Lc += v * v;
        Lc = std::sqrt(Lc);
        error(maxs / (Ls * h) - 1, 1e-8, "stress Lipschitz bound");
        error(maxc / (Lc * h) - 1, 1e-8, "tangent Lipschitz bound");
        ds.push_back(maxs);
        dc.push_back(maxc);
    }
    for (int i = 1; i < 3; ++i) {
        error(std::abs(ds[i - 1] / ds[i] - 2), .1, "stress ramp halving");
        error(std::abs(dc[i - 1] / dc[i] - 2), .1, "tangent ramp halving");
    }
}
template <class M>
void activation_test(M model) {
    WithActivation wrapped(model);
    Point p(wrapped);
    p.in.T_n = p.in.T_np1 = 345;
    for (auto& v : p.in.strain_np1)
        v = random(-.01, .01);
    p.in.strain_n = p.in.strain_np1;
    p.old[0] = 1;
    p.old[1] = 345;
    p.old[8] = 2;
    for (int j = 0; j < 6; ++j)
        p.old[2 + j] = p.in.strain_np1[j];
    p.ctx.clock = {2, 0, 0, 0, 0};
    p.eval();
    error(mandel_norm(p.out.stress), 1e-12 * 2e9 * .01, "activation zero stress");
    p.step();
    Vec6 de{.001, -.0003, .0002, 0, .0001, .0004};
    for (int j = 0; j < 6; ++j)
        p.in.strain_np1[j] += de[j];
    p.eval();
    error(relative(p.out.stress, mat6_vec(p.out.tangent, de)),
          1e-12,
          "activation incremental response");
    p.old[0] = 0;
    auto old = p.old;
    p.eval();
    require(p.old == old && p.next == old, "excluded state frozen");
    WithActivation soft(model, InactiveMode::SoftStiffness);
    Point q(soft);
    q.old = p.old;
    q.in = p.in;
    q.eval();
    require(q.next == q.old, "soft state frozen");
    q.old[0] = 1;
    for (int j = 0; j < 6; ++j)
        q.in.strain_np1[j] = q.old[2 + j];
    q.eval();
    error(mandel_norm(q.out.stress), 1e-12 * 2e9 * .01, "soft mode reactivation zero stress");
}
void t23() {
    ThermalStrainParams tp;
    tp.T_sf = 300;
    ThermalStrain th(tp, TemperatureTable({200, 500}, {{1e-5, 1e-5}}));
    activation_test(elastic(2e9, .25, th));
    activation_test(prony(VolumetricMode::ElasticBulk, {}, th));
    activation_test(Shrunk{elastic(2e9, .25, th), source()});
    auto m = make_material<Kokkos::HostSpace>(params(true, true, true));
    QuadratureStateStore<Kokkos::HostSpace> s(m->layout(), 2);
    m->init_state(s.trial());
    for (int j = 0; j < 2; ++j)
        s.trial()(0, j) = 0;
    s.commit();
    BlobStateWriter before;
    s.save(before);
    Soa<Real, Kokkos::HostSpace> strain("activation_strain", 6, 2);
    Kokkos::deep_copy(strain, .002);
    std::size_t points[] = {0, 1};
    activate<Kokkos::HostSpace>(s, points, {2, 345}, strain);
    Kokkos::fence();
    for (int j = 0; j < 2; ++j) {
        require(s.trial()(1, j) == 345, "activate writes trial");
        for (unsigned k = 9; k < s.layout().n_reals; ++k)
            require(s.trial()(k, j) == 0, "activation resets internals");
    }
    deactivate<Kokkos::HostSpace>(s, points);
    Kokkos::fence();
    s.discard();
    BlobStateWriter after;
    s.save(after);
    require(before.bytes() == after.bytes(), "activation discard bitwise rollback");
}
void t24pt() {
    Real h1 = .0007, h2 = .0011, E1 = 22e9, E2 = 130e9, nu1 = .3, nu2 = .27, alpha1 = 25e-6,
         alpha2 = 3e-6, dT = -100;
    Real M1 = E1 / (1 - nu1), M2 = E2 / (1 - nu2);
    auto point_stress = [&](Real E, Real nu, Real alpha) {
        ThermalStrainParams tp;
        tp.T_sf = 400;
        ThermalStrain th(tp, TemperatureTable({200, 500}, {{alpha, alpha}}));
        Point p(WithActivation{elastic(E, nu, th)});
        p.old[1] = 400;
        p.old[8] = 0;
        p.in.T_n = 400;
        p.in.T_np1 = 300;
        // Plane stress: eps33 eliminates sigma33 under biaxial constraint.
        p.in.strain_np1[2] = (1 + nu) / (1 - nu) * alpha * dT;
        p.eval();
        error(std::abs(p.out.stress[2]) / (E * alpha * 100), 1e-12, "plane stress condition");
        return p.out.stress[0];
    };
    Real s1 = point_stress(E1, nu1, alpha1), s2 = point_stress(E2, nu2, alpha2);
    close(s1, -M1 * alpha1 * dT);
    close(s2, -M2 * alpha2 * dT);
    // Exact force/moment release of two constant-eigenstrain layers, interface z=0.
    Real A = M1 * h1 + M2 * h2, B = (-M1 * h1 * h1 + M2 * h2 * h2) / 2,
         D = (M1 * h1 * h1 * h1 + M2 * h2 * h2 * h2) / 3;
    Real N = s1 * h1 + s2 * h2, M = (-s1 * h1 * h1 + s2 * h2 * h2) / 2;
    Real curvature = (B * N - A * M) / (A * D - B * B);
    Real analytic = 6 * M1 * M2 * h1 * h2 * (h1 + h2) * (alpha2 - alpha1) * dT /
                    (M1 * M1 * std::pow(h1, 4) + M2 * M2 * std::pow(h2, 4) +
                     2 * M1 * M2 * h1 * h2 * (2 * h1 * h1 + 3 * h1 * h2 + 2 * h2 * h2));
    close(curvature, analytic);
}
int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    id = argc > 1 ? argv[1] : "";
    try {
        if (id == "T1")
            t1();
        else if (id == "T2")
            t2();
        else if (id == "T4")
            t4();
        else if (id == "T5")
            t5();
        else if (id == "T6")
            t6();
        else if (id == "T7")
            t7();
        else if (id == "T8")
            t8();
        else if (id == "T9")
            t9();
        else if (id == "T10")
            t10();
        else if (id == "T11")
            t11();
        else if (id == "T12")
            t12();
        else if (id == "T13")
            t13();
        else if (id == "T15")
            t15();
        else if (id == "T16")
            t16(argv[2]);
        else if (id == "T17")
            t17();
        else if (id == "T18")
            t18();
        else if (id == "T19")
            t19();
        else if (id == "T20")
            t20();
        else if (id == "T21")
            t21();
        else if (id == "T22")
            t22();
        else if (id == "T23")
            t23();
        else if (id == "T24pt")
            t24pt();
        else
            require(false, "unknown test id");
        std::printf(
            "METRIC {\"id\":\"%s\",\"measured_error\":%.17g,\"tolerance\":%.17g,\"seed\":%u}\n",
            id.c_str(),
            measured,
            tolerance,
            seed);
    } catch (const std::exception& e) {
        require(false, e.what());
    }
}
