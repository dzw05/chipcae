#pragma once
#include "chipcae/material/temperature_table.hpp"

#include <stdexcept>
namespace chipcae::material {
// Owning immutable device snapshot. Construct on host, capture by value in kernels.
struct TableView {
    using Space = Kokkos::DefaultExecutionSpace::memory_space;
    Kokkos::View<const Real*, Space, Kokkos::MemoryTraits<Kokkos::RandomAccess>> t, values;
    int n = 0, columns = 0;
    Extrapolation extrapolation = Extrapolation::Error;
    TableView() = default;
    explicit TableView(const TemperatureTable& table)
        : n(table.num_temperatures()), columns(table.num_columns()),
          extrapolation(table.extrapolation()) {
        Kokkos::View<Real*, Space> a("table_T", n), b("table_values", n * columns);
        auto ah = Kokkos::create_mirror_view(a);
        auto bh = Kokkos::create_mirror_view(b);
        for (int i = 0; i < n; ++i)
            ah(i) = table.temperatures()[i];
        for (int c = 0; c < columns; ++c)
            for (int i = 0; i < n; ++i)
                bh(c * n + i) = table.columns()[c][i];
        Kokkos::deep_copy(a, ah);
        Kokkos::deep_copy(b, bh);
        t = a;
        values = b;
    }
    KOKKOS_INLINE_FUNCTION StatusCode evaluate(int c,
                                               Real T,
                                               Real& v,
                                               bool* clamped = nullptr) const {
        if (c < 0 || c >= columns || n == 0 || !Kokkos::isfinite(T))
            return StatusCode::Failed;
        if (T < t(0) || T > t(n - 1)) {
            if (extrapolation == Extrapolation::Error)
                return StatusCode::OutOfRange;
            if (clamped)
                *clamped = true;
        }
        if (T < t(0)) {
            v = values(c * n);
            return StatusCode::Ok;
        }
        if (T >= t(n - 1)) {
            v = values(c * n + n - 1);
            return StatusCode::Ok;
        }
        int i = 0;
        while (i + 1 < n && t(i + 1) <= T)
            ++i;
        v = values(c * n + i) +
            (T - t(i)) / (t(i + 1) - t(i)) * (values(c * n + i + 1) - values(c * n + i));
        return Kokkos::isfinite(v) ? StatusCode::Ok : StatusCode::Failed;
    }
    KOKKOS_INLINE_FUNCTION StatusCode integral(int c, Real a, Real b, Real& result) const {
        result = 0;
        Real sign = 1;
        if (b < a) {
            Real tmp = a;
            a = b;
            b = tmp;
            sign = -1;
        }
        Real unused;
        auto code = evaluate(c, a, unused);
        if (code != StatusCode::Ok)
            return code;
        code = evaluate(c, b, unused);
        if (code != StatusCode::Ok)
            return code;
        // Integrate each open interval using its midpoint: exact for affine alpha,
        // including one-sided values at duplicate knots.
        Real left = a;
        for (int i = 0; i <= n; ++i) {
            Real right = i < n ? t(i) : b;
            if (right <= left)
                continue;
            if (right > b)
                right = b;
            Real mid;
            code = evaluate(c, (left + right) * 0.5, mid);
            if (code != StatusCode::Ok)
                return code;
            result += (right - left) * mid;
            left = right;
            if (left >= b)
                break;
        }
        result *= sign;
        return Kokkos::isfinite(result) ? StatusCode::Ok : StatusCode::Failed;
    }
};
} // namespace chipcae::material
