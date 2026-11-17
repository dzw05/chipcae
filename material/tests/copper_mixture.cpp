#include "chipcae/material/copper_mixture.hpp"

#include "chipcae/material/activation.hpp"
#include "chipcae/material/expand_point_param.hpp"
#include "chipcae/material/material_params.hpp"

#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace chipcae::material;
using MS = Kokkos::HostSpace;
void check(bool b, const char* why) {
    if (!b)
        throw std::runtime_error(why);
}
void near(Real a, Real b, Real tol = 1e-12) {
    check(std::abs(a - b) <= tol * std::max({std::abs(a), std::abs(b), 1e-30}), "relative error");
}
MaterialParams card(bool copper) {
    MaterialParams p;
    p.model_id = "thermo_elastic@1";
    p.elastic_table =
        TemperatureTable({233, 533},
                         copper ? std::vector<std::vector<Real>>{{130e9, 100e9}, {.34, .32}}
                                : std::vector<std::vector<Real>>{{8e9, 2e9}, {.24, .28}});
    p.cte_table = TemperatureTable({233, 533},
                                   copper ? std::vector<std::vector<Real>>{{16e-6, 19e-6}}
                                          : std::vector<std::vector<Real>>{{35e-6, 65e-6}});
    p.thermal.T_sf = 300;
    p.thermal.T_ref = 273;
    p.thermal.cte_type = copper ? CteType::Secant : CteType::Tangent;
    return p;
}
ThermoElastic<MS> kernel(const MaterialParams& p) {
    return {p.elastic_symmetry, p.elastic_table, ThermalStrain<MS>{p.thermal, p.cte_table}};
}
PointInput input(Real f, Real T = 400) {
    PointInput in;
    in.T_n = 300;
    in.T_np1 = T;
    in.point_param = f;
    for (int i = 0; i < 6; ++i)
        in.strain_np1[i] = (i + 1) * 1e-4;
    return in;
}
PointOutput run(const auto& model, PointInput in) {
    PointOutput out;
    check(model.update(StepContext{}, in, {}, {}, out) == StatusCode::Ok, "update failed");
    return out;
}
void composition();
void limits() {
    auto a = kernel(card(false)), b = kernel(card(true));
    CopperMixture<MS> mix(a, b);
    Mat3 R{0, 0, 1, 1, 0, 0, 0, 1, 0};
    Real maximum = 0;
    for (Real f : {0., 1.})
        for (Real T : {233., 400., 533.})
            for (bool override_zero : {false, true}) {
                auto in = input(f, T);
                in.R = &R;
                in.override_T_sf = override_zero;
                in.T_sf = 350;
                auto x = run(mix, in), y = run(f == 0 ? a : b, in);
                for (int i = 0; i < 36; ++i) {
                    near(x.tangent[i], y.tangent[i], 1e-14);
                    maximum = std::max(maximum, std::abs(x.tangent[i] - y.tangent[i]));
                }
                for (int i = 0; i < 6; ++i) {
                    near(x.stress[i], y.stress[i], 1e-14);
                    near(x.thermal_strain[i], y.thermal_strain[i], 1e-14);
                }
            }
    for (Real f : {-1.,
                   1.01,
                   std::numeric_limits<Real>::quiet_NaN(),
                   std::numeric_limits<Real>::infinity()}) {
        PointOutput out;
        check(mix.update({}, input(f), {}, {}, out) == StatusCode::Failed &&
                  out.status.code == StatusCode::Failed,
              "invalid fraction accepted");
    }
    composition();
    std::cout << "endpoint max absolute tangent difference=" << maximum << '\n';
}
// Independent dense inverse by pivoted Gauss-Jordan, for checking engineering compliance.
Mat6 inverse(Mat6 a) {
    Mat6 b{};
    for (int i = 0; i < 6; ++i)
        b[6 * i + i] = 1;
    for (int i = 0; i < 6; ++i) {
        int pivot = i;
        for (int j = i + 1; j < 6; ++j)
            if (std::abs(a[6 * j + i]) > std::abs(a[6 * pivot + i]))
                pivot = j;
        for (int k = 0; k < 6; ++k) {
            std::swap(a[6 * i + k], a[6 * pivot + k]);
            std::swap(b[6 * i + k], b[6 * pivot + k]);
        }
        Real d = a[6 * i + i];
        for (int k = 0; k < 6; ++k) {
            a[6 * i + k] /= d;
            b[6 * i + k] /= d;
        }
        for (int j = 0; j < 6; ++j)
            if (j != i) {
                Real v = a[6 * j + i];
                for (int k = 0; k < 6; ++k) {
                    a[6 * j + k] -= v * a[6 * i + k];
                    b[6 * j + k] -= v * b[6 * i + k];
                }
            }
    }
    return b;
}
void laminate() {
    auto m = card(false), c = card(true);
    auto a = kernel(m), b = kernel(c);
    CopperMixture<MS> mix(a, b);
    Real max_error = 0;
    for (Real f : {.1, .5, .9})
        for (Real T : {233., 400., 533.}) {
            auto in = input(f, T);
            auto out = run(mix, in);
            auto S = inverse(out.tangent);
            Real Em, nm, Ec, nc;
            a.isotropic_properties(in, Em, nm);
            b.isotropic_properties(in, Ec, nc);
            Real Ex = f * Ec + (1 - f) * Em, Ez = 1 / (f / Ec + (1 - f) / Em),
                 n = f * nc + (1 - f) * nm;
            near(1 / S[0], Ex);
            near(1 / S[7], Ex);
            near(1 / S[14], Ez);
            max_error = std::max(max_error, std::abs(1 / S[14] / Ez - 1));
            Real Gm = Em / (2 * (1 + nm)), Gc = Ec / (2 * (1 + nc));
            near(1 / (2 * S[35]), f * Gc + (1 - f) * Gm);
            near(1 / (2 * S[21]), 1 / (f / Gc + (1 - f) / Gm));
            near(-S[2] / S[0], n);
            auto x = run(a, in), y = run(b, in);
            Real th = (f * Ec * y.thermal_strain[0] + (1 - f) * Em * x.thermal_strain[0]) / Ex;
            near(out.thermal_strain[0], th);
            near(out.thermal_strain[2],
                 f * (1 + nc) * y.thermal_strain[0] + (1 - f) * (1 + nm) * x.thermal_strain[0] -
                     n * th);
            in.override_T_sf = true;
            in.T_sf = T;
            out = run(mix, in);
            for (auto v : out.thermal_strain)
                check(v == 0, "thermal zero override");
            in = input(f, T);
            Mat3 R{0, 0, 1, 1, 0, 0, 0, 1, 0};
            in.R = &R;
            auto rotated = run(mix, in);
            auto expected = rotate_stiffness_global(run(mix, input(f, T)).tangent, R);
            for (int i = 0; i < 36; ++i)
                near(rotated.tangent[i], expected[i]);
        }
    std::cout << "laminate max relative normal modulus error=" << max_error << '\n';
}
void spd() {
    CopperMixture<MS> mix(kernel(card(false)), kernel(card(true)));
    Real minimum = 1e100;
    for (int i = 0; i <= 100; ++i)
        for (int t = 0; t <= 20; ++t) {
            auto C = run(mix, input(i / 100., 233 + 15 * t)).tangent;
            Mat6 L{};
            for (int j = 0; j < 6; ++j)
                for (int k = 0; k <= j; ++k) {
                    Real v = C[j * 6 + k];
                    for (int l = 0; l < k; ++l)
                        v -= L[j * 6 + l] * L[k * 6 + l];
                    if (j == k) {
                        check(v > 0, "non-SPD tangent");
                        minimum = std::min(minimum, v);
                        L[j * 6 + k] = std::sqrt(v);
                    } else
                        L[j * 6 + k] = v / L[k * 6 + k];
                }
        }
    std::cout << "SPD samples=2121 minimum Cholesky pivot=" << minimum << '\n';
}
void composition() {
    CopperMixture<MS> mix(kernel(card(false)), kernel(card(true)));
    WithActivation<CopperMixture<MS>> active(mix);
    Real old[9]{}, next[9]{};
    active.init_state({old, 1});
    old[1] = 400;
    old[8] = 1;
    for (Real f : {0., .5, 1.}) {
        auto in = input(f);
        PointOutput out;
        check(active.update({}, in, {old, 1}, {next, 1}, out) == StatusCode::Ok,
              "activation update");
        for (auto v : out.thermal_strain)
            check(v == 0, "activation thermal zero");
    }
    WithEigenstrains<CopperMixture<MS>, EquivalentCureShrinkage> eig(mix,
                                                                     EquivalentCureShrinkage{});
    std::vector<Real> a(eig.layout().n_reals + 1), b(a.size());
    eig.init_state({a.data(), 1});
    PointOutput out;
    auto in = input(.5);
    auto expected = run(mix, in);
    check(eig.update({}, in, {a.data(), 1}, {b.data(), 1}, out) == StatusCode::Ok,
          "eigenstrain wrapper");
    for (int i = 0; i < 6; ++i)
        near(out.stress[i], expected.stress[i]);
}
void reject() {
    auto m = card(false), c = card(true);
    MaterialParams opts;
    check(CopperMixture<MS>(kernel(m), kernel(c)).layout().n_reals == 0, "mixture has history");
    check(bool(make_copper_mixture<MS>(m, c, opts)), "runtime factory");
    m.kind = ModelKind::PronyViscoelastic;
    bool caught = false;
    try {
        make_copper_mixture<MS>(m, c, opts);
    } catch (const std::invalid_argument& e) {
        std::string s = e.what();
        caught = s.find("*_eq") != s.npos && s.find("Prony") != s.npos;
        std::cout << s << '\n';
    }
    check(caught, "missing visco diagnostic");
    m = card(false);
    m.elastic_symmetry = ElasticSymmetry::Orthotropic;
    caught = false;
    try {
        make_copper_mixture<MS>(m, c, opts);
    } catch (const std::invalid_argument&) {
        caught = true;
    }
    check(caught, "anisotropy accepted");
    opts.has_activation = true;
    auto wrapped = make_copper_mixture<MS>(card(false), c, opts);
    check(wrapped->layout().n_reals > 0, "activation wrapper missing");
}
void batch(bool mixture) {
    auto p = card(false);
    if (!mixture) {
        p.kind = ModelKind::PronyViscoelastic;
        p.prony = PronyHostParams{VolumetricMode::ElasticBulk, 2e9, .3, {{3e9, 1}}};
        p.bulk_table = TemperatureTable({233, 533}, {{5e9, 4e9}});
        p.shift = ShiftParams{ShiftType::Table, 300, 300, 0, 0, 0, {233, 533}, {0, 0}};
    }
    for (bool elastic : {false, true}) {
        if (elastic)
            p = card(false);
        auto model = mixture ? make_copper_mixture<MS>(card(false), card(true), MaterialParams{})
                             : make_material<MS>(p);
        constexpr int N = 5;
        Soa<Real, MS> strain("strain", 6, N), old("old", model->layout().n_reals, N),
            next("next", model->layout().n_reals, N);
        Vec1<Real, MS> T("T", N), phi("phi", N);
        Kokkos::deep_copy(strain, 1e-3);
        Kokkos::deep_copy(T, 400);
        Kokkos::deep_copy(phi, mixture ? .5 : .42);
        BatchInput<MS> in{};
        in.strain_n = strain;
        in.strain_np1 = strain;
        in.T_n = T;
        in.T_np1 = T;
        BatchOutput<MS> out{};
        out.stress = Soa<Real, MS>("stress", 6, N);
        out.tangent = Soa<Real, MS>("C", 36, N);
        out.thermal_strain = Soa<Real, MS>("th", 6, N);
        out.status = Vec1<Status, MS>("status", N);
        model->init_state(old);
        StepContext ctx;
        ctx.dt = .1;
        model->update_batch(ctx, in, old, next, out);
        Kokkos::fence();
        auto saved = Kokkos::create_mirror_view_and_copy(MS{}, out.stress);
        Soa<Real, MS> copy("copy", 6, N);
        Kokkos::deep_copy(copy, saved);
        Soa<Real, MS> saved_C("saved_C", 36, N), saved_th("saved_th", 6, N),
            saved_state("saved_state", next.extent(0), N);
        Kokkos::deep_copy(saved_C, out.tangent);
        Kokkos::deep_copy(saved_th, out.thermal_strain);
        Kokkos::deep_copy(saved_state, next);
        for (int count : {N - 1, N + 1}) {
            in.point_param = Vec1<Real, MS>("mismatch", count);
            model->update_batch(ctx, in, old, next, out);
            Kokkos::fence();
            for (int j = 0; j < N; ++j)
                check(out.status(j).code == StatusCode::Failed, "parameter extent accepted");
        }
        in.point_param = phi;
        model->update_batch(ctx, in, old, next, out);
        Kokkos::fence();
        for (int j = 0; j < N; ++j) {
            check(out.status(j).code == StatusCode::Ok, "batch status");
            for (int i = 0; i < 6; ++i)
                if (!mixture)
                    check(out.stress(i, j) == copy(i, j), "empty parameter changed regression");
        }
        if (!mixture) {
            for (int j = 0; j < N; ++j) {
                for (int i = 0; i < 36; ++i)
                    check(saved_C(i, j) == out.tangent(i, j), "empty parameter tangent regression");
                for (int i = 0; i < 6; ++i)
                    check(saved_th(i, j) == out.thermal_strain(i, j),
                          "empty parameter thermal regression");
                for (std::size_t i = 0; i < next.extent(0); ++i)
                    check(saved_state(i, j) == next(i, j), "empty parameter state regression");
            }
        }
        if (mixture) {
            auto pin = input(.5);
            for (auto& v : pin.strain_np1)
                v = 1e-3;
            auto direct = run(CopperMixture<MS>(kernel(card(false)), kernel(card(true))), pin);
            for (int i = 0; i < 6; ++i)
                near(out.stress(i, 0), direct.stress[i]);
            check(out.stress(0, 0) != copy(0, 0), "batch fraction ignored");
        }
    }
}
template <class Exec, class Space>
typename Kokkos::View<Real**, Space>::HostMirror execution() {
    CopperMixture<Space> mix(
        ThermoElastic<Space>{ElasticSymmetry::Isotropic,
                             card(false).elastic_table,
                             ThermalStrain<Space>{card(false).thermal, card(false).cte_table}},
        ThermoElastic<Space>{ElasticSymmetry::Isotropic,
                             card(true).elastic_table,
                             ThermalStrain<Space>{card(true).thermal, card(true).cte_table}});
    Kokkos::View<Real**, Space> result("result", 2121, 48);
    Kokkos::View<int*, Space> status("status", 2121);
    Kokkos::parallel_for(
        "mixture exec", Kokkos::RangePolicy<Exec>(0, 2121), KOKKOS_LAMBDA(int j) {
            PointInput in{};
            in.point_param = (j % 101) / 100.;
            in.T_np1 = 233 + 15 * (j / 101);
            for (int i = 0; i < 6; ++i)
                in.strain_np1[i] = (i + 1) * 1e-4;
            PointOutput out;
            status(j) = int(mix.update({}, in, {}, {}, out));
            for (int i = 0; i < 36; ++i)
                result(j, i) = out.tangent[i];
            for (int i = 0; i < 6; ++i) {
                result(j, 36 + i) = out.stress[i];
                result(j, 42 + i) = out.thermal_strain[i];
            }
        });
    Kokkos::fence();
    auto codes = Kokkos::create_mirror_view_and_copy(MS{}, status);
    for (int j = 0; j < 2121; ++j)
        check(codes(j) == int(StatusCode::Ok), "exec status");
    auto h = Kokkos::create_mirror_view(result);
    Kokkos::deep_copy(h, result);
    return h;
}
void exec(bool require_cuda = false) {
#if !defined(KOKKOS_ENABLE_CUDA)
    if (require_cuda) {
        std::cout << "SKIP: CUDA Kokkos/device runner unavailable\n";
        return;
    }
#else
    (void)require_cuda;
#endif
#if defined(KOKKOS_ENABLE_SERIAL) && defined(KOKKOS_ENABLE_OPENMP)
    auto a = execution<Kokkos::Serial, MS>(), b = execution<Kokkos::OpenMP, MS>();
    check(std::memcmp(a.data(), b.data(), a.size() * sizeof(Real)) == 0, "Serial/OpenMP differ");
    std::cout << "Serial/OpenMP bitwise equal: " << a.size() << " scalars\n";
#endif
#if defined(KOKKOS_ENABLE_CUDA)
    auto a_cuda = execution<Kokkos::DefaultHostExecutionSpace, MS>();
    auto c = execution<Kokkos::Cuda, Kokkos::CudaSpace>();
    for (std::size_t i = 0; i < a_cuda.size(); ++i)
        near(a_cuda.data()[i], c.data()[i]);
#endif
}
void expansion() {
    chipcae::mesh::HexMeshArrays mesh;
    mesh.axes[2] = {0, 1, 2, 3};
    // Nonlocal/reordered GIDs ensure expansion uses global columns, not local indices.
    mesh.elem_gid = {8, 0, 4, 7, 2, 3, 6, 1, 5};
    auto device = chipcae::mesh::upload<MS>(mesh);
    auto phi = chipcae::mesh::upload_vector<Real, MS>({.1, .5, .9}, "columns");
    auto expanded = expand_point_param(device, phi);
    auto host = Kokkos::create_mirror_view_and_copy(MS{}, expanded);
    for (std::size_t e = 0; e < mesh.elem_gid.size(); ++e)
        check(host(e) == phi(mesh.elem_gid[e] / 3), "column expansion");
}
int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    try {
        std::string which = argv[1];
        if (which == "expand")
            expansion();
        else if (which == "limits")
            limits();
        else if (which == "laminate")
            laminate();
        else if (which == "spd")
            spd();
        else if (which == "reject_visco")
            reject();
        else if (which == "empty")
            batch(false);
        else if (which == "exec_cuda") {
            exec(true);
        } else if (which == "exec") {
            exec();
            batch(true);
        } else
            return 2;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
