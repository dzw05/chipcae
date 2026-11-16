// chipcae/material/src/temperature_table.cpp
#include "chipcae/material/temperature_table.hpp"

#include <utility>

namespace chipcae::material {

TemperatureTable::TemperatureTable(std::vector<Real> temperatures,
                                   std::vector<std::vector<Real>> columns,
                                   Interpolation interpolation,
                                   Extrapolation extrapolation)
    : temperatures_(std::move(temperatures)), columns_(std::move(columns)),
      interpolation_(interpolation), extrapolation_(extrapolation) {}

bool TemperatureTable::strictly_increasing() const noexcept {
    for (std::size_t i = 1; i < temperatures_.size(); ++i)
        if (!(temperatures_[i] > temperatures_[i - 1]))
            return false;
    return true;
}

bool TemperatureTable::has_duplicate_temperatures() const noexcept {
    for (std::size_t i = 1; i < temperatures_.size(); ++i)
        if (temperatures_[i] == temperatures_[i - 1])
            return true;
    return false;
}

StatusCode TemperatureTable::evaluate(std::size_t column, Real T, Real& value) const noexcept {
    if (column >= columns_.size())
        return StatusCode::Failed;
    const auto& col = columns_[column];
    if (col.size() != temperatures_.size() || temperatures_.empty())
        return StatusCode::Failed;
    if (interpolation_ != Interpolation::Linear)
        return StatusCode::NotImplemented; // D6

    const std::size_t n = temperatures_.size();
    if (T <= temperatures_.front()) {
        if (T < temperatures_.front() && extrapolation_ == Extrapolation::Error)
            return StatusCode::OutOfRange;
        value = col.front();
        return StatusCode::Ok;
    }
    if (T >= temperatures_.back()) {
        if (T > temperatures_.back() && extrapolation_ == Extrapolation::Error)
            return StatusCode::OutOfRange;
        value = col.back();
        return StatusCode::Ok;
    }
    for (std::size_t i = 1; i < n; ++i) {
        if (T <= temperatures_[i]) {
            const Real t0 = temperatures_[i - 1];
            const Real t1 = temperatures_[i];
            const Real w = (T - t0) / (t1 - t0);
            value = col[i - 1] + w * (col[i] - col[i - 1]);
            return StatusCode::Ok;
        }
    }
    return StatusCode::OutOfRange;
}

} // namespace chipcae::material
