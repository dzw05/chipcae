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
using mesh::StackModel;
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
    std::optional<Real> resolved_z; // Resolved part attachment face, global z in metres.
};
struct FramePlacement {
    std::array<Real, 3> origin{0, 0, 0};
};
using Placement = std::variant<AttachPlacement, FramePlacement>;
struct PlacedPart {
    std::string id, material;
    std::vector<Slab> slabs; // Global coordinates with the placement transform already applied.
};
Expected<PlacedPart> place(std::vector<Slab> local, const Placement&, const StackModel& base);
struct MergeOptions {
    Real t_min, eps_weld, tau_a;
};
// Layers follow increasing z. Copied base regions retain their input order;
// appended regions follow input part order, then zero-based slab/section order.
// Hole-bridge fragments use deterministic fragment order within each section.
// Appended regions have body == PlacedPart::id and name <id>#<slab>#<section>.
// Copies across split layers and hole-bridge fragments share source identity;
// use (layer, body, name) to recover provenance, rather than vector position.
struct MergeResult {
    StackModel stack;
    std::vector<Defect> defects;
    std::int64_t welded_interfaces = 0;
};
Expected<MergeResult>
merge_parts(StackModel base, const std::vector<PlacedPart>&, const MergeOptions&);
} // namespace chipcae::geometry
