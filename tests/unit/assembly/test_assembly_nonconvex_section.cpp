#include "assembly_test_support.hpp"

#include <utility>

namespace {
using Polygon = std::vector<std::array<double, 2>>;

StackModel board() {
    auto b = base();
    b.outline.box = {-.1, -.1, .1, .1};
    return b;
}

SlabSection v_notch() {
    // Synthetic millimetre vertices converted to metres; analytic area is 2438 mm^2.
    return {Ring{{{.002, 0},
                  {.054, 0},
                  {.056, .002},
                  {.056, .042},
                  {.054, .044},
                  {.031, .044},
                  {.028, .038},
                  {.025, .044},
                  {.002, .044},
                  {0, .042},
                  {0, .002}}},
            {}};
}

SlabSection chamfered_l() {
    // Synthetic millimetre vertices converted to metres; analytic area is 1276 mm^2.
    return {Ring{{{.002, 0},
                  {.040, 0},
                  {.040, .020},
                  {.020, .020},
                  {.020, .044},
                  {.002, .044},
                  {0, .042},
                  {0, .002}}},
            {}};
}

PlacedPart lid(const std::string& id, const SlabSection& section) {
    auto p = part(id);
    p.slabs[0].sections = {section};
    return p;
}

std::vector<const Inclusion*> body_regions(const MergeResult& r, const std::string& body) {
    std::vector<const Inclusion*> found;
    for (const auto& layer : r.stack.layers)
        for (const auto& region : layer.regions)
            if (region.body == body)
                found.push_back(&region);
    return found;
}

void polygon_matches(const Inclusion& region, Polygon expected, double expected_area) {
    auto actual = region.footprint.polygon;
    check(!actual.empty(), "nonconvex section must retain a polygon");
    check(actual.size() == expected.size(), "normalized polygon vertex count");
    const double actual_area = area(actual);
    check(actual_area > 0, "nonconvex polygon must be CCW");
    check(std::abs(actual_area - expected_area) <= 1e-12 * expected_area,
          "nonconvex polygon analytic area (relative tolerance 1e-12)");
    auto canonicalize = [](Polygon& p) {
        std::rotate(p.begin(), std::min_element(p.begin(), p.end()), p.end());
    };
    canonicalize(actual);
    canonicalize(expected);
    for (size_t i = 0; i < actual.size(); ++i)
        for (size_t axis = 0; axis < 2; ++axis)
            check(std::abs(actual[i][axis] - expected[i][axis]) <= 1e-12,
                  "canonical polygon coordinate (absolute tolerance 1e-12 m)");

    std::array<double, 4> extrema{actual[0][0], actual[0][1], actual[0][0], actual[0][1]};
    for (const auto& p : actual) {
        extrema[0] = std::min(extrema[0], p[0]);
        extrema[1] = std::min(extrema[1], p[1]);
        extrema[2] = std::max(extrema[2], p[0]);
        extrema[3] = std::max(extrema[3], p[1]);
    }
    // The acceptance contract expressly requires exact box/polygon extrema equality.
    check(region.footprint.box == extrema, "box must equal polygon extrema exactly");
}

void no_defects(const MergeResult& r) {
    if (!r.defects.empty())
        throw std::runtime_error("expected no defects; got " + r.defects.front().defect_id + ": " +
                                 r.defects.front().details);
}

void single_region(const MergeResult& r,
                   const PlacedPart& p,
                   const Polygon& expected,
                   double expected_area) {
    no_defects(r);
    const auto regions = body_regions(r, p.id);
    check(regions.size() == 1, "single-slab body must emit exactly one region");
    check(regions[0]->body == p.id && regions[0]->material == p.material,
          "body and material must be retained");
    polygon_matches(*regions[0], expected, expected_area);
}

void exact_cut_defect(const MergeResult& r,
                      const std::string& id,
                      size_t slab,
                      const std::string& reason) {
    check(r.defects.size() == 1, "exactly one rejected-slab defect");
    const auto& d = r.defects[0];
    check(d.tier == Tier::C && d.reason == DefectReason::PART_NOT_PRISMATIC,
          "unsupported section retains C-tier PART_NOT_PRISMATIC");
    check(d.defect_id == id + "/cut_unsupported#" + std::to_string(slab),
          "exact unsupported slab defect ID");
    check(d.details == "cut_unsupported: " + reason, "exact unsupported section diagnostic");
    for (const auto& layer : r.stack.layers)
        for (const auto& region : layer.regions)
            check(!region.name.starts_with(id + "#" + std::to_string(slab) + "#"),
                  "rejected slab must emit no region");
}

void v_notch_case() {
    const auto p = lid("v-notch", v_notch());
    auto r = merge_parts(board(), {p}, options());
    check(bool(r), "V-notch merge succeeds");
    single_region(r.value(), p, v_notch().outer.pts, 2.438e-3);
}

void chamfered_l_case() {
    const auto p = lid("chamfered-l", chamfered_l());
    auto r = merge_parts(board(), {p}, options());
    check(bool(r), "chamfered-L merge succeeds");
    single_region(r.value(), p, chamfered_l().outer.pts, 1.276e-3);
}

void rotated_l_case() {
    auto b = board();
    auto p = lid("rotated-l", ell());
    AttachPlacement placement;
    placement.target = "layer:Core";
    placement.rot_z_deg = 15;
    auto placed = place(p.slabs, placement, b);
    check(bool(placed), "rotated-L placement succeeds");
    p.slabs = placed.value().slabs;

    // Independently apply the analytic rotation to the original ring.
    auto expected = ell().outer.pts;
    const double angle = std::acos(-1.) / 12;
    for (auto& point : expected) {
        const auto [x, y] = point;
        point = {std::cos(angle) * x - std::sin(angle) * y,
                 std::sin(angle) * x + std::cos(angle) * y};
    }
    auto r = merge_parts(b, {p}, options());
    check(bool(r), "rotated-L merge succeeds");
    single_region(r.value(), p, expected, 7e-4);
}

void repeated_slabs_case() {
    auto p = lid("repeated", v_notch());
    p.slabs.push_back({.002, .003, {v_notch()}});
    auto& ring = p.slabs[1].sections[0].outer.pts;
    std::reverse(ring.begin(), ring.end());
    std::rotate(ring.begin(), ring.begin() + 3, ring.end());
    ring.push_back(ring.front());
    ring.insert(ring.begin() + 1, ring.front());
    auto r = merge_parts(board(), {p}, options());
    check(bool(r), "equivalent nonconvex sections merge");
    no_defects(r.value());
    const auto regions = body_regions(r.value(), p.id);
    check(regions.size() == 2, "each equivalent slab retains its region");
    for (size_t slab = 0; slab < regions.size(); ++slab) {
        check(regions[slab]->name == p.id + "#" + std::to_string(slab) + "#0" &&
                  regions[slab]->material == p.material,
              "equivalent slab identity and material");
        polygon_matches(*regions[slab], v_notch().outer.pts, 2.438e-3);
    }
}

void collinear_case() {
    auto p = lid("collinear", v_notch());
    auto& ring = p.slabs[0].sections[0].outer.pts;
    ring.insert(ring.begin() + 1, {.028, 0});
    auto r = merge_parts(board(), {p}, options());
    check(bool(r), "forward-collinear midpoint merge succeeds");
    single_region(r.value(), p, v_notch().outer.pts, 2.438e-3);
}

void deterministic_case() {
    const auto p = lid("deterministic", v_notch());
    auto first = merge_parts(board(), {p}, options());
    auto second = merge_parts(board(), {p}, options());
    check(bool(first) && bool(second), "repeated merges succeed");
    single_region(first.value(), p, v_notch().outer.pts, 2.438e-3);
    single_region(second.value(), p, v_notch().outer.pts, 2.438e-3);
    same_regions(first.value(), second.value());
    for (size_t i = 0; i < first.value().stack.layers.size(); ++i) {
        const auto& a = first.value().stack.layers[i];
        const auto& b = second.value().stack.layers[i];
        check(a.name == b.name && a.thickness == b.thickness,
              "repeat layer names and thicknesses exactly");
    }
    check(first.value().defects.empty() && second.value().defects.empty(),
          "repeat identical empty diagnostics");
    check(first.value().welded_interfaces == second.value().welded_interfaces,
          "repeat weld count exactly");
}

void outline_snap_case() {
    auto b = board();
    b.outline.box = {0, 0, .056, .044};
    auto p = lid("snapped", v_notch());
    for (auto& point : p.slabs[0].sections[0].outer.pts)
        for (size_t axis = 0; axis < 2; ++axis) {
            if (point[axis] == b.outline.box[axis])
                point[axis] += 5e-13;
            else if (point[axis] == b.outline.box[axis + 2])
                point[axis] -= 5e-13;
        }
    auto r = merge_parts(b, {p}, options());
    check(bool(r), "outline-adjacent nonconvex merge succeeds");
    single_region(r.value(), p, v_notch().outer.pts, 2.438e-3);
    auto actual = body_regions(r.value(), p.id)[0]->footprint.polygon;
    auto expected = v_notch().outer.pts;
    std::rotate(actual.begin(), std::min_element(actual.begin(), actual.end()), actual.end());
    std::rotate(
        expected.begin(), std::min_element(expected.begin(), expected.end()), expected.end());
    for (size_t i = 0; i < expected.size(); ++i)
        for (size_t axis = 0; axis < 2; ++axis)
            if (expected[i][axis] == b.outline.box[axis])
                check(actual[i][axis] == b.outline.box[axis], "lower outline edge snaps exactly");
            else if (expected[i][axis] == b.outline.box[axis + 2])
                check(actual[i][axis] == b.outline.box[axis + 2],
                      "upper outline edge snaps exactly");
}

void bow_tie_case() {
    const auto p = lid("bow-tie", {Ring{{{0, 0}, {.004, .004}, {.004, 0}, {0, .004}}}, {}});
    invalid_input(merge_parts(board(), {p}, options()), "empty slabs or invalid geometry");
}

void holed_case() {
    auto b = board();
    auto p = lid("holed", {rect().outer, {rect(.001, .001, .002, .002).outer}});
    AttachPlacement placement;
    placement.target = "layer:Core";
    placement.rot_z_deg = 15;
    auto placed = place(p.slabs, placement, b);
    check(bool(placed), "holed rectangle placement succeeds");
    p.slabs = placed.value().slabs;
    auto r = merge_parts(b, {p}, options());
    check(bool(r), "rotated holed section returns diagnostics, not input error");
    exact_cut_defect(r.value(), p.id, 0, "holed non-rectilinear section");
    check(body_regions(r.value(), p.id).empty(), "holed body has no region");
}

void distinct_sections_case() {
    auto p = lid("distinct", v_notch());
    auto changed = v_notch();
    changed.outer.pts[5][0] = .030;
    p.slabs.push_back({.002, .003, {changed}});
    auto r = merge_parts(board(), {p}, options());
    check(bool(r), "distinct sections return diagnostics, not input error");
    exact_cut_defect(r.value(), p.id, 1, "multiple distinct cut sections");
    const auto regions = body_regions(r.value(), p.id);
    check(regions.size() == 1 && regions[0]->name == "distinct#0#0" &&
              regions[0]->material == p.material,
          "supported first slab remains present");
    polygon_matches(*regions[0], v_notch().outer.pts, 2.438e-3);
}
} // namespace

int main() {
    using Case = std::pair<const char*, void (*)()>;
    const std::array cases{Case{"V-notch", v_notch_case},
                           Case{"chamfered L", chamfered_l_case},
                           Case{"rotated L", rotated_l_case},
                           Case{"equivalent slabs", repeated_slabs_case},
                           Case{"collinear normalization", collinear_case},
                           Case{"deterministic merge", deterministic_case},
                           Case{"outline snapping", outline_snap_case},
                           Case{"bow tie rejection", bow_tie_case},
                           Case{"holed rectangle rejection", holed_case},
                           Case{"distinct sections rejection", distinct_sections_case}};
    int failures = 0;
    for (const auto& [name, test] : cases) {
        std::cerr << "CASE: " << name << '\n';
        const int failed = run(test);
        std::cerr << (failed ? "FAIL: " : "PASS: ") << name << '\n';
        failures += failed;
    }
    return failures == 0 ? 0 : 1;
}
