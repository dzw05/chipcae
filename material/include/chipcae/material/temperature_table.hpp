// chipcae/material/temperature_table.hpp —— 分段线性温度表（N5）
#pragma once

#include "chipcae/material/material_point.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace chipcae::material {

enum class Interpolation : std::uint8_t { Linear, Pchip, Constant };
enum class Extrapolation : std::uint8_t { Error, Clamp };

// 主机侧不可变表：temperatures 严格递增；columns[c] 与 temperatures 等长。
// 弹性/体积模量表要求连续；CTE 表允许同一温度出现两次表示 Tg 处跳变。
class TemperatureTable {
public:
    TemperatureTable() = default;
    TemperatureTable(std::vector<Real> temperatures,
                     std::vector<std::vector<Real>> columns,
                     Interpolation interpolation = Interpolation::Linear,
                     Extrapolation extrapolation = Extrapolation::Error);

    std::size_t num_temperatures() const noexcept { return temperatures_.size(); }
    std::size_t num_columns() const noexcept { return columns_.size(); }
    const std::vector<Real>& temperatures() const noexcept { return temperatures_; }
    Interpolation interpolation() const noexcept { return interpolation_; }
    Extrapolation extrapolation() const noexcept { return extrapolation_; }

    // 严格递增检查：重复温度点返回 false（CTE 表允许重复，调用方可关闭该检查）。
    bool strictly_increasing() const noexcept;
    bool has_duplicate_temperatures() const noexcept;

    // 分段线性插值。out_of_range 时按 extrapolation 处理：Clamp 截断，Error 返回 OutOfRange。
    // v0 只实现 linear；pchip / constant 返回 NotImplemented（D6）。
    StatusCode evaluate(std::size_t column, Real T, Real& value) const noexcept;

private:
    std::vector<Real> temperatures_;
    std::vector<std::vector<Real>> columns_;
    Interpolation interpolation_ = Interpolation::Linear;
    Extrapolation extrapolation_ = Extrapolation::Error;
};

} // namespace chipcae::material
