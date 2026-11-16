// chipcae/material/driver/single_point.hpp —— 单点材料驱动器（控应变 / 控应力）
#pragma once

#include "chipcae/material/material_batch.hpp"
#include "chipcae/material/state_store.hpp"

#include <Kokkos_Core.hpp>
#include <cstddef>

namespace chipcae::material::driver {

struct DriverResult {
    Vec6 strain{};
    Vec6 stress{};
    Mat6 tangent{};
    Vec6 thermal_strain{};
    Vec6 chemical_strain{};
    Status status{};
};

// 单积分点驱动器：内部持有一个 1 点 QuadratureStateStore，逐步提交。
class SinglePointDriver {
public:
    explicit SinglePointDriver(const IMaterial<Kokkos::HostSpace>& material);

    void reset();
    StepContext& context() noexcept { return ctx_; }

    // 给定本步总应变与温度，推进一个时间步（控应变）。
    DriverResult strain_control(const Vec6& strain_np1, Real T_np1, Real dt, Real t_n = 0.0);

    // 控应力：局部 Newton 迭代求应变（CHUNK 6）。
    DriverResult stress_control(
        const Vec6& stress_target, Real T_np1, Real dt, int max_iter = 30, Real tol = 1e-10);

    const QuadratureStateStore<Kokkos::HostSpace>& store() const noexcept { return store_; }
    QuadratureStateStore<Kokkos::HostSpace>& store() noexcept { return store_; }

private:
    const IMaterial<Kokkos::HostSpace>& material_;
    QuadratureStateStore<Kokkos::HostSpace> store_;
    StepContext ctx_{};
    Vec6 strain_prev_{};
    Real temperature_prev_ = 0;
};

} // namespace chipcae::material::driver
