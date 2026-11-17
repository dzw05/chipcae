#pragma once
#include "chipcae/geometry/part_slice.hpp"
#include "chipcae/schema/stack.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace chipcae::geometry {
enum class AttachSide { top, bottom };
struct Bond {
    std::string material;
    Real thickness = 0;
};
struct AttachPlacement {
    std::string target; // layer:<name> or part:<id>, without the side suffix.
    AttachSide side = AttachSide::top;
    Real gap = 0;
    std::optional<Bond> bond;
    std::array<Real, 2> xy{0, 0};
    Real rot_z_deg = 0;
    std::optional<Real> resolved_z; // Resolved part attachment face, in metres.
};
struct FramePlacement {
    std::array<Real, 3> origin{0, 0, 0};
};
using Placement = std::variant<AttachPlacement, FramePlacement>;
struct PlacedPart {
    std::string id, material;
    std::vector<Slab> slabs;
};
Expected<PlacedPart> place(std::vector<Slab> local, const Placement&, const mesh::StackModel& base);
struct MergeOptions {
    Real t_min, eps_weld, tau_a;
};
struct MergeResult {
    mesh::StackModel stack;
    std::vector<Defect> defects;
    std::int64_t welded_interfaces = 0;
};
Expected<MergeResult>
merge_parts(mesh::StackModel base, const std::vector<PlacedPart>&, const MergeOptions&);
} // namespace chipcae::geometry
