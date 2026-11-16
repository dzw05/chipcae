#include "chipcae/material/cte_convert.hpp"

#include "chipcae/material/table_view.hpp"

#include <cmath>
namespace chipcae::material {
namespace {
bool valid(const CteTable& t) {
    if (t.temperatures.size() < 2 || t.alpha.size() != t.temperatures.size() ||
        !std::isfinite(t.reference_temperature))
        return false;
    for (std::size_t i = 0; i < t.alpha.size(); ++i)
        if (!std::isfinite(t.alpha[i]) || !std::isfinite(t.temperatures[i]) ||
            (i && t.temperatures[i] <= t.temperatures[i - 1]))
            return false;
    return true;
}
} // namespace
StatusCode secant_to_tangent(const CteTable& t, std::vector<Real>& out) {
    out.clear();
    if (!valid(t))
        return StatusCode::Failed;
    for (std::size_t i = 0; i < t.alpha.size(); ++i) {
        auto j = i + 1 < t.alpha.size() ? i : i - 1;
        Real slope = (t.alpha[j + 1] - t.alpha[j]) / (t.temperatures[j + 1] - t.temperatures[j]);
        out.push_back(t.alpha[i] + (t.temperatures[i] - t.reference_temperature) * slope);
    }
    return StatusCode::Ok;
}
StatusCode tangent_to_secant(const CteTable& t, std::vector<Real>& out) {
    out.clear();
    if (!valid(t))
        return StatusCode::Failed;
    // Host-only integration, with constant endpoint extension.
    TemperatureTable table(t.temperatures, {t.alpha}, Interpolation::Linear, Extrapolation::Clamp);
    auto primitive = [&](Real T) {
        Real a = t.reference_temperature, b = T, sign = 1, total = 0;
        if (b < a) {
            std::swap(a, b);
            sign = -1;
        }
        Real l = a;
        for (std::size_t i = 0; i <= t.alpha.size(); ++i) {
            Real r = i < t.alpha.size() ? t.temperatures[i] : b;
            if (r <= l)
                continue;
            r = std::min(r, b);
            Real v;
            table.evaluate(0, (l + r) * 0.5, v);
            total += (r - l) * v;
            l = r;
            if (l >= b)
                break;
        }
        return sign * total;
    };
    for (Real T : t.temperatures) {
        Real v;
        if (T == t.reference_temperature)
            table.evaluate(0, T, v);
        else
            v = primitive(T) / (T - t.reference_temperature);
        out.push_back(v);
    }
    return StatusCode::Ok;
}
StatusCode secant_to_tangent(const CteTable& t, Real T, Real& out) {
    if (!valid(t) || !std::isfinite(T))
        return StatusCode::Failed;
    TemperatureTable table(t.temperatures, {t.alpha}, Interpolation::Linear, Extrapolation::Clamp);
    Real alpha;
    table.evaluate(0, T, alpha);
    Real slope = 0;
    if (T >= t.temperatures.front() && T <= t.temperatures.back()) {
        std::size_t j = 0;
        while (j + 2 < t.alpha.size() && t.temperatures[j + 1] <= T)
            ++j;
        slope = (t.alpha[j + 1] - t.alpha[j]) / (t.temperatures[j + 1] - t.temperatures[j]);
    }
    out = alpha + (T - t.reference_temperature) * slope;
    return std::isfinite(out) ? StatusCode::Ok : StatusCode::Failed;
}
StatusCode tangent_to_secant(const CteTable& t, Real T, Real& out) {
    if (!valid(t) || !std::isfinite(T))
        return StatusCode::Failed;
    TemperatureTable table(t.temperatures, {t.alpha}, Interpolation::Linear, Extrapolation::Clamp);
    if (T == t.reference_temperature)
        return table.evaluate(0, T, out);
    Real a = std::min(T, t.reference_temperature), b = std::max(T, t.reference_temperature),
         total = 0, left = a;
    for (std::size_t i = 0; i <= t.alpha.size(); ++i) {
        Real right = i < t.alpha.size() ? t.temperatures[i] : b;
        if (right <= left)
            continue;
        right = std::min(right, b);
        Real v;
        table.evaluate(0, (left + right) * 0.5, v);
        total += (right - left) * v;
        left = right;
        if (left >= b)
            break;
    }
    out = total / (b - a);
    return std::isfinite(out) ? StatusCode::Ok : StatusCode::Failed;
}

} // namespace chipcae::material
