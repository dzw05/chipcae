// tests/t_smoke.cpp —— 编译 / 链接 / Kokkos 初始化冒烟测试
#include "chipcae/material/activation.hpp"
#include "chipcae/material/eigenstrain.hpp"
#include "chipcae/material/mandel.hpp"
#include "chipcae/material/material_params.hpp"
#include "chipcae/material/state_io.hpp"
#include "chipcae/material/thermo_elastic.hpp"
#include "single_point.hpp"

#include <Kokkos_Core.hpp>
#include <cstdio>
#include <span>

using namespace chipcae::material;

static_assert(MaterialModel<ThermoElastic>);
static_assert(MaterialModel<WithActivation<ThermoElastic>>);
static_assert(MaterialModel<WithEigenstrains<ThermoElastic, EquivalentCureShrinkage>>);
static_assert(EigenstrainSource<EquivalentCureShrinkage>);

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    std::printf("chipcae_material smoke: Kokkos initialised (%d threads)\n",
                Kokkos::DefaultExecutionSpace().concurrency());

    const Vec6 e{1e-3, 2e-3, 3e-3, 0.0, 0.0, 0.0};
    const Vec6 s{1.0, 2.0, 3.0, 0.0, 0.0, 0.0};
    const Real dot = mandel_inner(e, s);
    const Real trace = mandel_trace(e);
    std::printf("chipcae_material smoke: sigma:eps=%g trace=%g\n", dot, trace);

    const Vec6 dev = mandel_dev(e);
    if (Kokkos::abs(mandel_trace(dev)) > 1e-15) {
        std::printf("FAIL: deviator trace is not zero\n");
        return 1;
    }

    MaterialParams params;
    params.kind = ModelKind::ThermoElastic;
    const auto mat = make_material<Kokkos::HostSpace>(params);
    if (!mat || mat->model_id() != "thermo_elastic@1") {
        std::printf("FAIL: factory did not build thermo_elastic\n");
        return 1;
    }

    // 状态 save/restore（POD 字节块 + layout hash）
    static const StateField fields[1] = {{"x", 0, 1, true}};
    const StateLayout layout{1, std::span<const StateField>(fields, 1), 0x1234ull};
    QuadratureStateStore<Kokkos::HostSpace> store(layout, 2);
    store.trial()(0, 0) = 3.5;
    store.trial()(0, 1) = -1.25;
    store.commit();
    BlobStateWriter writer;
    store.save(writer);
    QuadratureStateStore<Kokkos::HostSpace> restored(layout, 2);
    BlobStateReader reader(writer.bytes());
    restored.restore(reader);
    if (restored.committed()(0, 0) != 3.5 || restored.committed()(0, 1) != -1.25) {
        std::printf("FAIL: state save/restore round-trip\n");
        return 1;
    }

    // 驱动器可实例化；本构返回 Ok。
    driver::SinglePointDriver drive(*mat);
    const Vec6 eps{1e-4, 0.0, 0.0, 0.0, 0.0, 0.0};
    const auto res = drive.strain_control(eps, 300.0, 1.0);
    if (res.status.code != StatusCode::Ok) {
        std::printf("FAIL: expected Ok from constitutive kernel\n");
        return 1;
    }

    std::printf("chipcae_material smoke: OK\n");
    return 0;
}
