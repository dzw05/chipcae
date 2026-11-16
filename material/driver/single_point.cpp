// chipcae/material/driver/single_point.cpp
#include "single_point.hpp"

namespace chipcae::material::driver {

SinglePointDriver::SinglePointDriver(const IMaterial<Kokkos::HostSpace>& material)
    : material_(material), store_(material.layout(), 1) {}

void SinglePointDriver::reset() {
    store_.discard();
    strain_prev_ = Vec6{};
}

DriverResult
SinglePointDriver::strain_control(const Vec6& strain_np1, Real T_np1, Real dt, Real t_n) {
    DriverResult result;
    ctx_.t_n = t_n;
    ctx_.dt = dt;
    ctx_.kin = Kinematics::SmallStrain;
    ctx_.need_tangent = true;
    ctx_.need_dstress_dT = false;

    Soa<Real, Kokkos::HostSpace> s_n("strain_n", 6, 1);
    Soa<Real, Kokkos::HostSpace> s_np1("strain_np1", 6, 1);
    Vec1<Real, Kokkos::HostSpace> Tn_view("T_n", 1);
    Vec1<Real, Kokkos::HostSpace> Tnp1_view("T_np1", 1);
    Soa<Real, Kokkos::HostSpace> R("R", 0, 0);

    for (int i = 0; i < 6; ++i) {
        s_n(i, 0) = strain_prev_[i];
        s_np1(i, 0) = strain_np1[i];
    }
    Tn_view(0) = T_np1;
    Tnp1_view(0) = T_np1;

    BatchInput<Kokkos::HostSpace> in{};
    in.strain_n = s_n;
    in.strain_np1 = s_np1;
    in.T_n = Tn_view;
    in.T_np1 = Tnp1_view;
    in.R = R;

    BatchOutput<Kokkos::HostSpace> out{};
    out.stress = Soa<Real, Kokkos::HostSpace>("stress", 6, 1);
    out.tangent = Soa<Real, Kokkos::HostSpace>("tangent", 36, 1);
    out.thermal_strain = Soa<Real, Kokkos::HostSpace>("thermal_strain", 6, 1);
    out.chemical_strain = Soa<Real, Kokkos::HostSpace>("chemical_strain", 6, 1);
    out.dstress_dT = Soa<Real, Kokkos::HostSpace>("dstress_dT", 0, 0);
    out.dissipation = Vec1<Real, Kokkos::HostSpace>("dissipation", 0);
    out.status = Vec1<Status, Kokkos::HostSpace>("status", 1);

    material_.update_batch(ctx_, in, store_.committed(), store_.trial(), out);

    for (int i = 0; i < 6; ++i) {
        result.stress[i] = out.stress(i, 0);
        result.thermal_strain[i] = out.thermal_strain(i, 0);
        result.chemical_strain[i] = out.chemical_strain(i, 0);
    }
    for (int i = 0; i < 36; ++i)
        result.tangent[i] = out.tangent(i, 0);
    result.strain = strain_np1;
    result.status = out.status(0);

    if (result.status.code == StatusCode::Ok) {
        store_.commit();
        strain_prev_ = strain_np1;
    }
    return result;
}

DriverResult SinglePointDriver::stress_control(
    const Vec6& stress_target, Real T_np1, Real dt, int max_iter, Real tol) {
    (void)stress_target;
    (void)T_np1;
    (void)dt;
    (void)max_iter;
    (void)tol;
    DriverResult result;
    result.status = Status{StatusCode::NotImplemented, 1.0}; // TODO(K3): 局部 Newton
    return result;
}

} // namespace chipcae::material::driver
