// chipcae/material/driver/single_point.cpp
#include "single_point.hpp"

#include <cmath>

namespace chipcae::material::driver {

SinglePointDriver::SinglePointDriver(const IMaterial<Kokkos::HostSpace>& material)
    : material_(material),
      store_(material.layout(), 1, material.parameter_hash(), material.material_id()) {
    reset();
}

void SinglePointDriver::reset() {
    material_.init_state(store_.trial());
    store_.commit();
    strain_prev_ = Vec6{};
    temperature_prev_ = 0;
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
    Tn_view(0) = temperature_prev_ > 0 ? temperature_prev_ : T_np1;
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
        temperature_prev_ = T_np1;
    }
    return result;
}

DriverResult SinglePointDriver::stress_control(
    const Vec6& stress_target, Real T_np1, Real dt, int max_iter, Real tol) {
    Vec6 guess = strain_prev_;
    // Keep the committed handle fixed throughout Newton, accepting only the final iterate.
    auto prev = strain_prev_;
    Real oldT = temperature_prev_;
    DriverResult result;
    Real initial_scale = 1;
    for (int iter = 0; iter < max_iter; ++iter) {
        result = strain_control(guess, T_np1, dt);
        if (result.status.code != StatusCode::Ok)
            return result;
        store_.commit(); // undo handle swap performed by strain_control
        strain_prev_ = prev;
        temperature_prev_ = oldT;
        Real a[6][7]{};
        Real residual = 0, scale = 1;
        for (int i = 0; i < 6; ++i) {
            a[i][6] = stress_target[i] - result.stress[i];
            residual += a[i][6] * a[i][6];
            scale += stress_target[i] * stress_target[i];
            for (int j = 0; j < 6; ++j)
                a[i][j] = result.tangent[6 * i + j];
        }
        if (iter == 0)
            initial_scale = std::max(scale, residual);
        if (std::sqrt(residual) <= tol * std::sqrt(initial_scale)) {
            store_.commit();
            strain_prev_ = guess;
            temperature_prev_ = T_np1;
            return result;
        }
        for (int i = 0; i < 6; ++i) {
            int pivot = i;
            for (int j = i + 1; j < 6; ++j)
                if (std::abs(a[j][i]) > std::abs(a[pivot][i]))
                    pivot = j;
            if (a[pivot][i] == 0) {
                result.status.code = StatusCode::Failed;
                return result;
            }
            for (int j = i; j < 7; ++j)
                std::swap(a[i][j], a[pivot][j]);
            Real d = a[i][i];
            for (int j = i; j < 7; ++j)
                a[i][j] /= d;
            for (int k = 0; k < 6; ++k)
                if (k != i) {
                    Real f = a[k][i];
                    for (int j = i; j < 7; ++j)
                        a[k][j] -= f * a[i][j];
                }
        }
        for (int i = 0; i < 6; ++i)
            guess[i] += a[i][6];
    }
    result.status.code = StatusCode::Failed;
    return result;
}

} // namespace chipcae::material::driver
