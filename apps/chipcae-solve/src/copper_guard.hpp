#pragma once

#include "chipcae/schema/stack.hpp"
#include "chipcae/solve/case_loader.hpp"

namespace chipcae::solve::detail {
inline void require_planar_metal_copper(const chipcae::mesh::Layer& layer) {
    if (!layer.copper)
        return;
    const auto& copper = *layer.copper;
    if (copper.role != "metal")
        throw InputError(ErrorCode::INPUT_INVALID,
                         "INPUT_INVALID: layer " + layer.name + " copper role " + copper.role +
                             " not supported until solve-via-vertical-mixing");
    if (copper.orientation != "planar")
        throw InputError(ErrorCode::INPUT_INVALID,
                         "INPUT_INVALID: layer " + layer.name + " copper orientation " +
                             copper.orientation + " not supported until solve-via-vertical-mixing");
}
} // namespace chipcae::solve::detail
