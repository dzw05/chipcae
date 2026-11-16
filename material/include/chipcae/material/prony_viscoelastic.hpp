// chipcae/material/prony_viscoelastic.hpp —— 各向同性线性粘弹性（ModelKind::PronyViscoelastic）
//
// CHUNK 1 只提供类型、签名与可链接的占位实现（返回 NotImplemented）。
#pragma once

#include "chipcae/material/material_point.hpp"

#include <string_view>

namespace chipcae::material {

class PronyViscoelastic {
public:
    static constexpr std::string_view model_id = "prony_viscoelastic@1";
    static constexpr bool tangent_symmetric = true;

    static constexpr bool supports(Kinematics) { return true; }

    StateLayout layout() const {
        StateLayout out{};
        out.n_reals = 0; // TODO(K3): 6·N_G + 8（默认模式）/ 7·N + 8（常 ν 模式）
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

static_assert(MaterialModel<PronyViscoelastic>);

} // namespace chipcae::material
