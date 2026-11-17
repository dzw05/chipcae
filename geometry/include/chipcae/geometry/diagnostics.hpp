#pragma once

#include "chipcae/core/types.hpp"
#include "chipcae/geometry/import_thresholds.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace chipcae::geometry {
enum class StepDefectKind : std::uint16_t {
    Gap,
    OpenEdge,
    DuplicateFace,
    FlippedFace,
    NonManifoldEdge,
    SmallEdge,
    SmallFace,
    SliverFace,
    WireSelfIntersection,
    SolidOverlap,
    OpenShell,
    NearLayerInterface,
    UnitSuspect,
    FaceSelfIntersection,
    InvalidGeometry,
    ToleranceExceeded
};
struct StepDefect {
    std::string defect_id;
    StepDefectKind kind{};
    Tier tier = Tier::C;
    Real measured = 0;
    std::string step_entity;
    Index face_index = 0;
    std::string body_name;
    std::array<Real, 6> bbox{};
    std::array<Real, 3> where{};
    std::string action;
    std::string status; // Original BRepCheck/ArgumentAnalyzer status, when applicable.
};
} // namespace chipcae::geometry
