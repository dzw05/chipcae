// chipcae/material/shift.hpp —— 移位函数（WLF / Arrhenius / table / 拼接）
#pragma once

#include "chipcae/material/table_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace chipcae::material {

namespace detail {
// Test-first placeholder for the host-only portable Ei helper. No callers in production yet.
inline Real expint(Real) {
    return std::numeric_limits<Real>::quiet_NaN();
}
} // namespace detail

// CODATA 2018 气体常数 [J/(mol·K)]
inline constexpr Real kGasConstant = 8.314462618;

enum class ShiftType : std::uint8_t { WLF, Arrhenius, Table, PiecewiseWlfArrhenius };

// 主机侧不可变参数；ShiftFunction 构造时 deep_copy 到只读 View。
struct ShiftParams {
    ShiftType type = ShiftType::WLF;
    Real T_ref = 0.0;              // 参考温度 [K]
    Real T_g = 0.0;                // 拼接温度只取 glass_transition.T_g [K]
    Real C1 = 0.0;                 // WLF
    Real C2 = 0.0;                 // WLF [K]
    Real E_a = 0.0;                // Arrhenius 活化能 [J/mol]
    std::vector<Real> table_T;     // log10 aT 对 T 的节点 [K]
    std::vector<Real> table_log10; // 节点上的 log10 aT
};

// Immutable device parameters; host vectors never enter a captured kernel object.
template <class MS>
class ShiftFunction {
public:
    ShiftFunction() = default;
    explicit ShiftFunction(const ShiftParams& p)
        : type_(p.type), ref_(p.T_ref), tg_(p.T_g), c1_(p.C1), c2_(p.C2), ea_(p.E_a) {
        if (!(ref_ > 0))
            throw std::invalid_argument("shift.reference_temperature must be positive");
        if ((type_ == ShiftType::Arrhenius || type_ == ShiftType::PiecewiseWlfArrhenius) &&
            !(ea_ > 0))
            throw std::invalid_argument("shift.activation_energy must be positive");
        if (type_ == ShiftType::PiecewiseWlfArrhenius && !(tg_ > 0))
            throw std::invalid_argument("glass_transition.T_g must be positive");
        if (type_ == ShiftType::Table)
            table_ = TableView<MS>(TemperatureTable(p.table_T, {p.table_log10}));
        Real v;
        if ((type_ == ShiftType::Table
                 ? TemperatureTable(p.table_T, {p.table_log10}).evaluate(0, ref_, v)
                 : evaluate(ref_, v)) != StatusCode::Ok)
            throw std::invalid_argument("invalid shift reference");
    }
    KOKKOS_INLINE_FUNCTION StatusCode evaluate(Real T, Real& log10_aT) const noexcept {
        Real a, b;
        auto code = raw(T, a);
        if (code != StatusCode::Ok)
            return code;
        code = raw(ref_, b);
        log10_aT = a - b;
        return code == StatusCode::Ok && Kokkos::isfinite(log10_aT) ? StatusCode::Ok
               : code == StatusCode::Ok                             ? StatusCode::Failed
                                                                    : code;
    }
    KOKKOS_INLINE_FUNCTION StatusCode reduced_time_increment(Real a,
                                                             Real b,
                                                             Real dt,
                                                             Real& dxi) const noexcept {
        dxi = 0;
        if (!Kokkos::isfinite(dt) || dt < 0)
            return StatusCode::Failed;
        if (dt == 0)
            return StatusCode::Ok;
        if (a == b) {
            Real v;
            auto code = rate(a, v);
            dxi = dt * v;
            return code == StatusCode::Ok && !Kokkos::isfinite(dxi) ? StatusCode::Failed : code;
        }
        Real lo = Kokkos::fmin(a, b), hi = Kokkos::fmax(a, b), left = lo;
        // Split at every knot and the splice before adaptive integration.
        for (int k = 0; k <= table_.n + 1; ++k) {
            Real right = hi;
            if (type_ == ShiftType::Table && k < table_.n)
                right = table_.t(k);
            else if (type_ == ShiftType::PiecewiseWlfArrhenius && k == 0)
                right = tg_;
            if (right <= left)
                continue;
            if (right > hi)
                right = hi;
            Real val;
            auto code = integrate(left, right, val);
            if (code != StatusCode::Ok)
                return code;
            dxi += val;
            left = right;
            if (left >= hi)
                break;
        }
        dxi *= dt / (hi - lo);
        return Kokkos::isfinite(dxi) ? StatusCode::Ok : StatusCode::Failed;
    }
    // Host-only E1 reference: Ei(-z) = -E1(z), for positive physical coefficients.
    StatusCode reduced_time_reference(Real a, Real b, Real dt, Real& result) const {
        if (type_ == ShiftType::Table)
            return reduced_time_increment(a, b, dt, result);
        result = 0;
        if (dt < 0 || !std::isfinite(dt))
            return StatusCode::Failed;
        Real logref;
        auto code = raw(ref_, logref);
        if (code != StatusCode::Ok)
            return code;
        if (a == b)
            return reduced_time_increment(a, b, dt, result);
        Real lo = std::min(a, b), hi = std::max(a, b);
        auto primitive = [&](Real T, bool arr) {
            constexpr Real ln10 = 2.302585092994045684;
            Real u, B, A;
            if (arr) {
                u = T;
                B = ea_ / kGasConstant;
                Real base = 0, anchor = ref_;
                if (type_ == ShiftType::PiecewiseWlfArrhenius) {
                    anchor = tg_;
                    base = -c1_ * (tg_ - ref_) / (c2_ + tg_ - ref_);
                }
                A = ln10 * (logref - base) + B / anchor;
            } else {
                u = c2_ + T - ref_;
                B = ln10 * c1_ * c2_;
                A = ln10 * (c1_ + logref);
            }
            if (B == 0)
                return std::exp(A) * u;
            return std::exp(A) * (u * std::exp(-B / u) + B * std::expint(-B / u));
        };
        Real mid = type_ == ShiftType::PiecewiseWlfArrhenius ? std::clamp(tg_, lo, hi) : lo;
        if (type_ == ShiftType::Arrhenius)
            result = primitive(hi, true) - primitive(lo, true);
        else {
            if (mid > lo)
                result += primitive(mid, true) - primitive(lo, true);
            if (hi > mid)
                result += primitive(hi, false) - primitive(mid, false);
        }
        result *= dt / (hi - lo);
        return std::isfinite(result) ? StatusCode::Ok : StatusCode::Failed;
    }

private:
    ShiftType type_ = ShiftType::WLF;
    Real ref_ = 300, tg_ = 300, c1_ = 0, c2_ = 1e30, ea_ = 0;
    TableView<MS> table_{};
    KOKKOS_INLINE_FUNCTION StatusCode raw(Real T, Real& v) const noexcept {
        if (!(T > 0) || !Kokkos::isfinite(T))
            return StatusCode::OutOfRange;
        constexpr Real ln10 = 2.302585092994045684;
        if (type_ == ShiftType::Table)
            return table_.evaluate(0, T, v);
        if (type_ == ShiftType::Arrhenius) {
            v = ea_ / (kGasConstant * ln10) * (1 / T - 1 / ref_);
            return StatusCode::Ok;
        }
        Real w = T;
        if (type_ == ShiftType::PiecewiseWlfArrhenius && T < tg_)
            w = tg_;
        Real den = c2_ + w - ref_;
        if (!(den > 0))
            return StatusCode::OutOfRange;
        v = -c1_ * (w - ref_) / den;
        if (w != T)
            v += ea_ / (kGasConstant * ln10) * (1 / T - 1 / tg_);
        return Kokkos::isfinite(v) ? StatusCode::Ok : StatusCode::Failed;
    }
    KOKKOS_INLINE_FUNCTION StatusCode rate(Real T, Real& v) const noexcept {
        Real log;
        auto code = evaluate(T, log);
        if (code != StatusCode::Ok)
            return code;
        v = Kokkos::exp(-2.302585092994045684 * log);
        return Kokkos::isfinite(v) ? StatusCode::Ok : StatusCode::CutStep;
    }
    KOKKOS_INLINE_FUNCTION StatusCode gauss(Real a, Real b, Real& value) const noexcept {
        Real m = (a + b) * 0.5, h = (b - a) * 0.5, v0, v1, v2;
        auto code = rate(m, v0);
        if (code != StatusCode::Ok)
            return code;
        code = rate(m - h * 0.774596669241483377, v1);
        if (code != StatusCode::Ok)
            return code;
        code = rate(m + h * 0.774596669241483377, v2);
        if (code != StatusCode::Ok)
            return code;
        value = h * (8.0 / 9 * v0 + 5.0 / 9 * (v1 + v2));
        return StatusCode::Ok;
    }
    KOKKOS_INLINE_FUNCTION StatusCode integrate(Real a, Real b, Real& value) const noexcept {
        Real starts[21], ends[21], coarse[21];
        int depth[21], top = 0;
        starts[0] = a;
        ends[0] = b;
        depth[0] = 0;
        auto code = gauss(a, b, coarse[0]);
        if (code != StatusCode::Ok)
            return code;
        value = 0;
        while (top >= 0) {
            Real l = starts[top], r = ends[top], old = coarse[top], m = (l + r) * 0.5;
            int d = depth[top--];
            Real x, y;
            code = gauss(l, m, x);
            if (code != StatusCode::Ok)
                return code;
            code = gauss(m, r, y);
            if (code != StatusCode::Ok)
                return code;
            if (Kokkos::abs(x + y - old) <= 1e-10 * Kokkos::abs(x + y)) {
                value += x + y;
                continue;
            }
            if (d >= 20)
                return StatusCode::CutStep;
            ++top;
            starts[top] = m;
            ends[top] = r;
            coarse[top] = y;
            depth[top] = d + 1;
            ++top;
            starts[top] = l;
            ends[top] = m;
            coarse[top] = x;
            depth[top] = d + 1;
        }
        return StatusCode::Ok;
    }
};
} // namespace chipcae::material
