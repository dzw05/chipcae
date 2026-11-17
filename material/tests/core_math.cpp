#include "../../tests/unit/cte_contract/cte_baseline.hpp"
#include "chipcae/material/cte_convert.hpp"
#include "chipcae/material/prony_viscoelastic.hpp"
#include "chipcae/material/schema.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace chipcae::material;
void check(bool pass, const char* name) {
    if (!pass) {
        std::fprintf(stderr, "FAIL core_math: %s\n", name);
        std::exit(1);
    }
}
bool near(Real a, Real b, Real tol = 1e-12) {
    return std::abs(a - b) <= tol * std::max({1e-30, std::abs(a), std::abs(b)});
}
int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    TemperatureTable cte({250, 300, 300, 400},
                         {{1e-5, 2e-5, 4e-5, 6e-5}},
                         Interpolation::Linear,
                         Extrapolation::Clamp);
    ThermalStrainParams tp;
    tp.T_sf = 250;
    ThermalStrain<Kokkos::HostSpace> thermal(tp, cte);
    Vec6 th;
    check(thermal.evaluate(350, th) == StatusCode::Ok && near(th[0], 0.003),
          "CTE exact integral across jump");
    thermal.evaluate(250, th);
    check(th[0] == 0, "CTE zero at T_sf");
    thermal.evaluate(350, 350, th);
    check(th[0] == 0, "CTE activation override");
    thermal.evaluate(300 - 1e-7, th);
    Real left = th[0];
    thermal.evaluate(300 + 1e-7, th);
    check(std::abs(th[0] - left) < 7e-12, "CTE continuous at jump");
    const auto& sec = test::sec;
    std::vector<Real> converted;
    check(secant_to_tangent(sec, converted) == StatusCode::Ok && near(converted[1], 3e-5),
          "secant derivative conversion");
    const auto& tan = test::tan;
    check(tangent_to_secant(tan, converted) == StatusCode::Ok && near(converted[1], 2e-5),
          "tangent integral conversion");
    Real exact;
    check(tangent_to_secant(tan, test::tangent_to_secant_temperature, exact) == StatusCode::Ok &&
              near(exact, test::tangent_to_secant_expected),
          "off-knot exact inverse CTE conversion");
    check(secant_to_tangent(sec, test::secant_to_tangent_temperature, exact) == StatusCode::Ok &&
              near(exact, test::secant_to_tangent_expected),
          "off-knot exact CTE derivative");
    ThermoElastic<Kokkos::HostSpace> elastic(
        ElasticSymmetry::Isotropic, TemperatureTable({250, 400}, {{2e9, 1e9}, {0.25, 0.25}}));
    PointInput in;
    in.T_n = in.T_np1 = 300;
    in.strain_np1 = {1e-3, 0, 0, 0, 0, 2e-3};
    StepContext ctx;
    PointOutput out;
    check(elastic.update(ctx, in, {nullptr, 1}, {nullptr, 1}, out) == StatusCode::Ok,
          "elastic status");
    check(near(out.stress[5], (2e9 - (50.0 / 150) * 1e9) / 1.25 * 2e-3), "Mandel shear");
    auto baseline = out;
    in.strain_n = {99, 99, 99, 99, 99, 99};
    elastic.update(ctx, in, {nullptr, 1}, {nullptr, 1}, out);
    check(out.stress == baseline.stress, "elastic total strain form");
    ctx.need_dstress_dT = true;
    check(elastic.update(ctx, in, {nullptr, 1}, {nullptr, 1}, out) == out.status.code &&
              out.status.code == StatusCode::NotImplemented,
          "temperature derivative status");
    ctx.need_dstress_dT = false;
    for (int j = 0; j < 6; ++j) {
        Real h = 1e-8;
        auto pert = in;
        pert.strain_np1[j] += h;
        PointOutput plus, minus;
        elastic.update(ctx, pert, {nullptr, 1}, {nullptr, 1}, plus);
        pert.strain_np1[j] -= 2 * h;
        elastic.update(ctx, pert, {nullptr, 1}, {nullptr, 1}, minus);
        for (int i = 0; i < 6; ++i)
            check(std::abs((plus.stress[i] - minus.stress[i]) / (2 * h) -
                           baseline.tangent[i * 6 + j]) < 1e-7 * 2e9,
                  "elastic tangent FD");
    }
    ShiftParams sp;
    sp.type = ShiftType::PiecewiseWlfArrhenius;
    sp.T_ref = 320;
    sp.T_g = 300;
    sp.C1 = 4;
    sp.C2 = 100;
    sp.E_a = 30000;
    ShiftFunction<Kokkos::HostSpace> shift(sp);
    Real val;
    shift.evaluate(320, val);
    check(val == 0, "shift normalization");
    Real dxi, reference;
    check(shift.reduced_time_increment(280, 340, 2, dxi) == StatusCode::Ok,
          "shift crossing quadrature");
    check(shift.reduced_time_reference(280, 340, 2, reference) == StatusCode::Ok &&
              near(dxi, reference, 1e-10),
          "shift E1 reference");
    for (Real invalid_dt : {-1.,
                            std::numeric_limits<Real>::infinity(),
                            -std::numeric_limits<Real>::infinity(),
                            std::numeric_limits<Real>::quiet_NaN()}) {
        check(shift.reduced_time_reference(280, 340, invalid_dt, val) == StatusCode::Failed,
              "reference rejects negative or non-finite dt");
    }
    check(shift.reduced_time_reference(320, 320, 2, val) == StatusCode::Ok && near(val, 2.),
          "reference equal endpoints at reference temperature");
    check(shift.reduced_time_reference(280, 340, 0, val) == StatusCode::Ok && val == 0,
          "reference zero dt");
    shift.reduced_time_increment(340, 280, 2, val);
    check(near(val, dxi), "shift direction symmetry");
    sp.T_ref = 280;
    ShiftFunction<Kokkos::HostSpace> below(sp);
    below.evaluate(280, val);
    check(val == 0, "shift normalization below splice");
    sp.type = ShiftType::Table;
    sp.T_ref = 300;
    sp.table_T = {250, 300, 350};
    sp.table_log10 = {1, 0, -2};
    ShiftFunction<Kokkos::HostSpace> tableShift(sp);
    check(tableShift.reduced_time_increment(250, 350, 1, val) == StatusCode::Ok &&
              near(val, ((1 - 0.1) / std::log(10.) + (100 - 1) / (2 * std::log(10.))) / 2, 1e-10),
          "table shift knot splitting");
    for (auto mode : {VolumetricMode::ElasticBulk, VolumetricMode::ConstantPoissonFromE}) {
        PronyHostParams pp{VolumetricMode::ElasticBulk};
        pp.mode = mode;
        pp.infinity = 2;
        pp.terms = {{3, 2}};
        pp.poisson = 0.25;
        PronyViscoelastic<Kokkos::HostSpace> model(pp, TemperatureTable({250, 400}, {{5, 5}}));
        Real old[32]{}, next[32]{};
        model.init_state({old, 1});
        in.strain_n = {};
        in.strain_np1 = {0, 0, 0, 0, 0, 0.01};
        in.T_n = in.T_np1 = 300;
        ctx.dt = 0;
        check(model.update(ctx, in, {old, 1}, {next, 1}, out) == StatusCode::Ok, "Prony zero dt");
        Real factor = mode == VolumetricMode::ElasticBulk ? 1 : 2.5;
        check(near(out.stress[5], 0.1 / factor), "Prony instantaneous shear");
        for (int i = 0; i < 32; ++i)
            old[i] = next[i];
        ctx.dt = 2;
        ctx.need_dissipation = true;
        check(model.update(ctx, in, {old, 1}, {next, 1}, out) == StatusCode::Ok &&
                  out.dissipation >= 0,
              "Prony dissipation");
        check(near(out.stress[5], 2 * (2 + 3 * std::exp(-1.)) * 0.01 / factor), "Prony relaxation");
        ctx.dt = 1e8;
        check(model.update(ctx, in, {old, 1}, {next, 1}, out) == StatusCode::Ok &&
                  near(out.stress[5], 0.04 / factor),
              "Prony large x");
        Kokkos::View<Real**, Kokkos::LayoutRight, Kokkos::HostSpace> states("state", 32, 2);
        Kokkos::View<PointOutput*, Kokkos::HostSpace> outputs("out", 2);
        auto copy = model;
        auto input = in;
        auto context = ctx;
        Kokkos::parallel_for(
            "core_math_host",
            Kokkos::RangePolicy<Kokkos::DefaultHostExecutionSpace>(0, 2),
            KOKKOS_LAMBDA(int i) {
                copy.init_state({&states(0, i), 2});
                copy.update(context, input, {&states(0, i), 2}, {&states(16, i), 2}, outputs(i));
            });
        Kokkos::fence();
        check(outputs(0).status.code == StatusCode::Ok, "Prony HostSpace kernel");
    }
    // Exercise actual batch adapter initialization with nonzero memory and N > 1.
    PronyHostParams bp{VolumetricMode::ElasticBulk};
    bp.infinity = 2;
    bp.terms = {{3, 2}};
    PronyViscoelastic<Kokkos::HostSpace> bm(bp, TemperatureTable({250, 400}, {{5, 5}}));
    MaterialAdapter<PronyViscoelastic<Kokkos::HostSpace>, Kokkos::HostSpace> adapter(bm);
    Soa<Real, Kokkos::HostSpace> bs("batch_state", 14, 2), bn("batch_next", 14, 2),
        strain("batch_strain", 6, 2);
    Kokkos::deep_copy(bs, 123.0);
    adapter.init_state(bs);
    Kokkos::fence();
    for (int k = 0; k < 14; ++k)
        for (int j = 0; j < 2; ++j)
            check(bs(k, j) == 0, "batch SoA initialization");
    Vec1<Real, Kokkos::HostSpace> temperatures("temperatures", 2);
    Kokkos::deep_copy(temperatures, 300.0);
    Kokkos::deep_copy(strain, 0.0);
    strain(5, 0) = 0.01;
    strain(5, 1) = 0.02;
    BatchInput<Kokkos::HostSpace> bi;
    bi.strain_n = strain;
    bi.strain_np1 = strain;
    bi.T_n = temperatures;
    bi.T_np1 = temperatures;
    BatchOutput<Kokkos::HostSpace> bo;
    bo.stress = Soa<Real, Kokkos::HostSpace>("stress", 6, 2);
    bo.thermal_strain = Soa<Real, Kokkos::HostSpace>("thermal", 6, 2);
    bo.status = Vec1<Status, Kokkos::HostSpace>("status", 2);
    ctx.dt = 0;
    ctx.need_dissipation = false;
    adapter.update_batch(ctx, bi, bs, bn, bo);
    Kokkos::fence();
    check(near(bo.stress(5, 0), 0.1) && near(bo.stress(5, 1), 0.2),
          "batch distinct point response");
    for (int j = 0; j < 2; ++j) {
        Real state[14]{}, trial[14]{};
        PointInput point;
        point.T_n = point.T_np1 = 300;
        point.strain_np1[5] = strain(5, j);
        PointOutput po;
        bm.update(ctx, point, {state, 1}, {trial, 1}, po);
        for (int k = 0; k < 14; ++k)
            check(trial[k] == bn(k, j), "batch state equals point state");
    }
    for (auto symmetry : {ElasticSymmetry::TransverselyIsotropic, ElasticSymmetry::Orthotropic}) {
        TemperatureTable constants =
            symmetry == ElasticSymmetry::TransverselyIsotropic
                ? TemperatureTable({300}, {{10.}, {5.}, {0.2}, {0.1}, {2.}})
                : TemperatureTable({300},
                                   {{10.}, {8.}, {5.}, {0.2}, {0.1}, {0.15}, {3.}, {2.}, {1.}});
        ThermoElastic<Kokkos::HostSpace> anisotropic(symmetry, constants);
        PointInput point;
        point.T_np1 = 300;
        point.strain_np1 = {0.01, -0.02, 0.03, 0.04, 0.05, 0.06};
        PointOutput local, global;
        anisotropic.update(ctx, point, {nullptr, 1}, {nullptr, 1}, local);
        Real angle = 0.7;
        Mat3 R{std::cos(angle), -std::sin(angle), 0, std::sin(angle), std::cos(angle), 0, 0, 0, 1};
        point.strain_np1 = mat6_vec(mat6_transpose(mandel_rotation_matrix(R)), point.strain_np1);
        point.R = &R;
        anisotropic.update(ctx, point, {nullptr, 1}, {nullptr, 1}, global);
        Vec6 expected = mat6_vec(mat6_transpose(mandel_rotation_matrix(R)), local.stress);
        for (int k = 0; k < 6; ++k)
            check(std::abs(global.stress[k] - expected[k]) < 1e-14, "anisotropic frame rotation");
    }
    const std::string card =
        R"({"id":"test","schema_version":"1","unit_system":"SI","density":1000,"model":"thermo_elastic","provenance":{"source_type":"illustrative","confidence":"illustrative"},"elastic":{"symmetry":"isotropic","table":{"temperatures":[250,400],"columns":{"E":[1e9,2e9],"nu":[0.3,0.3]},"interpolation":"linear","extrapolation":"error"}},"thermal_expansion":{"cte_type":"tangent","symmetry":"isotropic","stress_free_temperature":300,"table":{"temperatures":[250,400],"columns":{"alpha":[1e-5,2e-5]},"interpolation":"linear","extrapolation":"error"}},"chemical_shrinkage":{"model":"equivalent_isotropic","post_gel_shrinkage":{"value":0.002,"measure":"volumetric"},"schedule":{"cure_step_ids":["cure"]}}})";
    check(validate_material_json(card).ok && parse_material_json(card).cure_step_ids.size() == 1,
          "JSON parsed parameters");
    for (const auto& ids : {"[]", "[\"a\",\"b\"]", "\"cure\""}) {
        auto bad = card;
        auto at = bad.find("[\"cure\"]");
        bad.replace(at, 8, ids);
        check(!validate_material_json(bad).ok, "P5 invalid binding list");
    }
    auto bad = card;
    bad.replace(bad.find("250,400"), 7, "250,250");
    check(!validate_material_json(bad).ok, "duplicate modulus knots rejected");
    auto prony_card = card;
    auto elastic_begin = prony_card.find("\"elastic\":");
    auto elastic_end = prony_card.find("\"thermal_expansion\":");
    prony_card.erase(elastic_begin, elastic_end - elastic_begin);
    auto model_at = prony_card.find("thermo_elastic");
    prony_card.replace(model_at, 14, "prony_viscoelastic");
    prony_card.pop_back();
    prony_card +=
        R"(,"glass_transition":{"T_g":300,"method":"DSC"},"viscoelastic":{"symmetry":"isotropic","reference_temperature":300,"volumetric_mode":"constant_poisson_from_E","poisson_ratio":0.46,"tensile":{"infinity":2,"terms":[{"modulus":3,"tau":2}]},"shift":{"type":"Arrhenius","activation_energy":30000}}})";
    check(validate_material_json(prony_card).ok, "Prony parser");
    auto parsed = parse_material_json(prony_card);
    check(!parsed.warnings.empty(), "high Poisson warning");
    auto factory = make_material<Kokkos::HostSpace>(parsed);
    check(!factory->approximation_notes().empty(), "adapter approximation metadata");
    auto missing_mode = prony_card;
    auto mode_at = missing_mode.find("\"volumetric_mode\":\"constant_poisson_from_E\",");
    missing_mode.erase(mode_at,
                       std::string("\"volumetric_mode\":\"constant_poisson_from_E\",").size());
    check(!validate_material_json(missing_mode).ok, "missing volumetric mode rejected");
    std::puts("core_math: OK (deterministic cases, no random seed)");
}
