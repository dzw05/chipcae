// chipcae/material/thermo_elastic.hpp —— 温度相关线弹性（ModelKind::ThermoElastic）
//
// CHUNK 1 只提供类型、签名与可链接的占位实现（返回 NotImplemented）。
#pragma once

#include "chipcae/material/material_point.hpp"

#include <string_view>

namespace chipcae::material {

class ThermoElastic {
public:
    static constexpr std::string_view model_id = "thermo_elastic@1";
    static constexpr bool tangent_symmetric = true;

    static constexpr bool supports(Kinematics) { return true; }

    StateLayout layout() const {
        StateLayout out{};
        out.n_reals = 0; // 全量割线型：无内变量（组合器另行加前缀状态）
        return out;
    }

    void init_state(StateRef) const noexcept {}

    StatusCode update(const StepContext&,
                      const PointInput&,
                      ConstStateRef,
                      StateRef,
                      PointOutput& out) const noexcept {
        out = PointOutput{};
        out.status = Status{StatusCode::NotImplemented, 1.0};
        return StatusCode::NotImplemented; // TODO(K3)
    }
};

static_assert(MaterialModel<ThermoElastic>);

} // namespace chipcae::material
