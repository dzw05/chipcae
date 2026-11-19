// chipcae/material/src/temperature_table.cpp
#include "chipcae/material/temperature_table.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace chipcae::material {

TemperatureTable::TemperatureTable(std::vector<Real> temperatures,
                                   std::vector<std::vector<Real>> columns,
                                   Interpolation interpolation,
                                   Extrapolation extrapolation)
    : temperatures_(std::move(temperatures)), columns_(std::move(columns)),
      interpolation_(interpolation), extrapolation_(extrapolation) {
    if (temperatures_.empty() || columns_.empty())
        throw std::invalid_argument("temperature table is empty");
    if (interpolation != Interpolation::Linear)
        throw std::invalid_argument("interpolation: only linear implemented");
    for (std::size_t i = 0; i < temperatures_.size(); ++i) {
        if (!std::isfinite(temperatures_[i]) || (i && temperatures_[i] < temperatures_[i - 1]))
            throw std::invalid_argument("temperatures must be nondecreasing and finite");
        if (i > 1 && temperatures_[i] == temperatures_[i - 2])
            throw std::invalid_argument("at most two knots at a CTE jump");
    }
    for (const auto& col : columns_) {
        if (col.size() != temperatures_.size())
            throw std::invalid_argument("column length mismatch");
        for (Real v : col)
            if (!std::isfinite(v))
                throw std::invalid_argument("nonfinite table value");
    }
}

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
    if (column >= columns_.size() || !std::isfinite(T))
        return StatusCode::Failed;
    const auto& col = columns_[column];
    if (temperatures_.size() == 1) {
        value = col.front();
        return StatusCode::Ok;
    }
    if (T < temperatures_.front() || T > temperatures_.back()) {
        if (extrapolation_ == Extrapolation::Error)
            return StatusCode::OutOfRange;
        clamped_warning_ = true;
    }
    if (T < temperatures_.front()) {
        value = col.front();
        return StatusCode::Ok;
    }
    if (T >= temperatures_.back()) {
        value = col.back();
        return StatusCode::Ok;
    }
    std::size_t i = 0;
    while (i + 1 < temperatures_.size() && temperatures_[i + 1] <= T)
        ++i;
    value = col[i] + (T - temperatures_[i]) / (temperatures_[i + 1] - temperatures_[i]) *
                         (col[i + 1] - col[i]);
    return StatusCode::Ok;
}
} // namespace chipcae::material
