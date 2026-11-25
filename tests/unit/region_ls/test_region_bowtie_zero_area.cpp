#include "chipcae/mesh/cut/detail/convex_pieces.hpp"
#include "chipcae/mesh/cut/detail/outline_nonconvex.hpp"
#include "chipcae/mesh/cut/outline_cut.hpp"
#include "region_cut_support.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <type_traits>
#include <vector>

namespace {
using namespace chipcae;
using namespace chipcae::mesh;
using namespace chipcae::mesh::cut;
namespace geometry = chipcae::mesh::cut::detail;
using geometry::Ring2;
using sp5::require;

// Recursive serializer and rollback sentinels follow consumer_error in
// test_region_cut_unions.cpp. Include every HexMeshArrays field, including the
// additive copper-area report, without comparing padding or floating values.
struct Bits {
    std::vector<std::uint64_t> words;

    template <class T>
        requires std::is_arithmetic_v<T>
    void add(T value) {
        if constexpr (std::is_floating_point_v<T>)
            words.push_back(std::bit_cast<std::uint64_t>(value));
        else
            words.push_back(static_cast<std::uint64_t>(value));
    }
    void add(const std::string& value) {
        add(value.size());
        for (unsigned char c : value)
            add(c);
    }
    template <class T, std::size_t N>
    void add(const std::array<T, N>& values) {
        for (const auto& value : values)
            add(value);
    }
    template <class T>
    void add(const std::vector<T>& values) {
        add(values.size());
        for (const auto& value : values)
            add(value);
    }
    template <class K, class V>
    void add(const std::map<K, V>& values) {
        add(values.size());
        for (const auto& [key, value] : values) {
            add(key);
            add(value);
        }
    }
    void add(const AttributeRecord& value) {
        add(value.layer);
        add(value.region);
        add(value.material);
        add(value.body);
    }
    void add(const AttributeTable& value) {
        add(value.layers);
        add(value.entries);
        add(value.references);
        add(value.materials);
        add(value.bodies);
        add(value.boundary_names);
    }
    void add(const SnapReport& value) {
        add(value.axis);
        add(value.from);
        add(value.to);
        add(value.displacement);
    }
    void add(const HexMeshArrays::InterfaceReport& value) {
        add(value.name);
        add(value.side_a_bodies);
        add(value.side_b_bodies);
        add(value.aligned_faces);
        add(value.cut_faces);
        add(value.aligned_area);
        add(value.cut_area);
    }
    void add(const HexMeshArrays::GridReport::CopperArea& value) {
        add(value.polygons);
        add(value.raster);
    }
    void add(const HexMeshArrays::GridReport& value) {
        add(value.levels);
        add(value.rule);
        add(value.N_L);
        add(value.auto_selected);
        add(value.padding);
        add(value.max_aspect_ratio);
        add(value.level_axes);
        add(value.column_ranks);
        add(value.warnings);
        add(value.partition_imbalance);
        add(value.void_cells);
        add(value.copper_area_m2);
    }
    void add(const HexMeshArrays& value) {
        add(value.vertices);
        add(value.hex);
        add(value.attribute);
        add(value.bdr_quad);
        add(value.bdr_attribute);
        add(value.elem_gid);
        add(value.node_gid);
        add(value.partitioning);
        add(value.node_layer);
        add(value.copper_layer_of_layer);
        add(value.copper_phi);
        add(value.node_column);
        add(value.attr_frame);
        add(value.nodes);
        add(value.internal_interfaces);
        add(value.order);
        add(value.preparation_report);
        add(value.grid_report);
        add(value.attr_table);
        add(value.face_sets);
        add(value.pending_cut);
        add(value.snapped);
        add(value.axes);
    }
};

template <class T>
Bits snapshot(const T& value) {
    Bits bits;
    bits.add(value);
    return bits;
}

void exact_error(const Error& actual, std::string_view path, std::string_view reason) {
    const auto message = "expected " + std::string(path) + " / " + std::string(reason) + "; got " +
                         actual.path + " / " + actual.reason;
    require(actual.path == path && actual.reason == reason, message.c_str());
}

struct InvalidRing {
    std::string name;
    Ring2 ring;
    std::string reason;
};

// Expectations follow I11 cutgeom-nc/1.0.4: equal bowtie lobes cancel the
// translated fan area, but their crossing topology is still non-simple.
// C1-C3/L1 collapse during cleanup; L2 has finite coordinates and nonfinite
// fan area. Those guards must take precedence over simplicity testing.
std::vector<InvalidRing> invalid_rings() {
    const Ring2 b0{{0, 0}, {.02, .02}, {.02, 0}, {0, .02}};
    auto b0r = b0;
    std::reverse(b0r.begin(), b0r.end());
    std::rotate(b0r.begin(), b0r.begin() + 1, b0r.end());
    b0r.push_back(b0r.front());
    constexpr double B = 1.5e308;
    const auto nan = std::bit_cast<Real>(std::uint64_t{0x7ff8000000000451ULL});
    const auto inf = std::numeric_limits<Real>::infinity();
    return {
        {"B0", b0, "lid: self-intersecting polygon"},
        {"H0", {{0, 0}, {.02, 0}, {0, .02}, {.02, .02}}, "lid: self-intersecting polygon"},
        {"B0r", b0r, "lid: self-intersecting polygon"},
        {"B1",
         {{.015625, .0078125}, {.03125, .0234375}, {.03125, .0078125}, {.015625, .0234375}},
         "lid: self-intersecting polygon"},
        // N1 has nonzero signed area and must use the existing simplicity checks.
        {"N1", {{0, 0}, {.02, .02}, {.02, 0}, {0, .01}}, "lid: self-intersecting polygon"},
        {"C1", {{0, 0}, {.01, 0}, {.02, 0}}, "lid: degenerate polygon"},
        {"C2", {{0, 0}, {.02, 0}, {.01, 0}}, "lid: degenerate polygon"},
        {"C3", {{0, 0}, {.005, 0}, {.01, 0}, {.02, 0}, {.01, 0}}, "lid: degenerate polygon"},
        {"L1", {{0, 0}, {.01, 0}, {.02, .01}, {.01, 0}}, "lid: degenerate polygon"},
        {"L2", {{-B, -B}, {B, B}, {B, -B}, {-B, B}}, "lid: degenerate polygon"},
        {"tiny-square",
         {{0, 0}, {1e-14, 0}, {1e-14, 1e-14}, {0, 1e-14}},
         "lid: degenerate polygon"},
        {"nan-square", {{0, 0}, {.02, 0}, {nan, .02}, {0, .02}}, "lid: degenerate polygon"},
        {"inf-square", {{0, 0}, {.02, 0}, {.02, inf}, {0, .02}}, "lid: degenerate polygon"},
    };
}

struct SimpleRing {
    std::string name;
    Ring2 canonical;
};

std::vector<SimpleRing> simple_rings() {
    // Handwritten CCW oracles, starting at minimum (y,x). The L shape is the
    // existing synthetic fixture from test_convex_pieces.cpp.
    return {{"square", {{0, 0}, {.02, 0}, {.02, .02}, {0, .02}}},
            {"triangle", {{0, 0}, {.02, 0}, {.02, .02}}},
            {"L-shape", {{0, 0}, {.02, 0}, {.02, .0137}, {.0113, .0137}, {.0113, .02}, {0, .02}}}};
}

StackModel region_stack(const Ring2& ring) {
    auto stack = sp5b::chamfer_stack();
    auto& region = stack.layers[0].regions[0];
    region.name = region.body = "lid";
    region.footprint.polygon = ring;
    return stack;
}

void region_error(const InvalidRing& test) {
    const auto result = region_cut_planes(region_stack(test.ring), sp5b::registered_mesh());
    require(!result, "invalid region must fail region_cut_planes");
    exact_error(result.error(), "CUT_UNSUPPORTED", test.reason);
}

PieceUnion square_union(BodyId body) {
    PieceUnion result;
    result.body = body;
    result.ring = {{0, 0}, {.02, 0}, {.02, .02}, {0, .02}};
    result.pieces = {{{body, {0, -1, 0}, 0},
                      {body, {1, 0, 0}, .02},
                      {body, {0, 1, 0}, .02},
                      {body, {-1, 0, 0}, 0}}};
    result.internal = {{false, false, false, false}};
    return result;
}

void sentinels(HexMeshArrays& mesh) {
    const auto nan = std::bit_cast<Real>(std::uint64_t{0x7ff8000000000451ULL});
    mesh.copper_phi = {-0., nan};
    mesh.nodes = {-0., nan};
    mesh.copper_layer_of_layer = {-1};
    mesh.snapped.push_back({'x', -0., 0., -0.});
    mesh.grid_report.warnings.push_back("rollback sentinel");
    mesh.grid_report.copper_area_m2["sentinel"] = {-0., nan};
}

constexpr std::array<std::string_view, 4> entries{
    "mutable-build", "const-build", "public-outline", "general-outline"};

// Each entry gets a fresh valid grid and a valid convex piece. Only the source
// ring is replaced. Check rollback before classification so an expected red
// reason assertion cannot hide a mutation of the caller's mesh.
void consumer_case(const InvalidRing* test, std::size_t entry) {
    const auto stack = region_stack(simple_rings()[0].canonical);
    auto mesh = sp5::grid(stack, 8);
    auto piece = square_union(mesh.attr_table.bodies.at("lid"));
    if (test) {
        piece.ring = test->ring;
        sentinels(mesh);
    }
    const CutGeometry input{{}, {piece}};
    const auto before = snapshot(mesh);
    auto check_result = [&](const auto& result) {
        if (!test) {
            require(static_cast<bool>(result), "valid convex PieceUnion control must succeed");
            return;
        }
        require(snapshot(mesh).words == before.words,
                "failed consumer must preserve every mesh field and NaN/signed-zero bit");
        require(!result, "invalid source ring must fail the consumer");
        exact_error(result.error(), "CUT_UNSUPPORTED", test->reason);
    };
    if (entry < 2) {
        const auto result =
            entry == 0 ? build_cut_layer(mesh, input, {})
                       : build_cut_layer(static_cast<const HexMeshArrays&>(mesh), input, {});
        check_result(result);
    } else {
        const auto result =
            entry == 2 ? apply_outline_cut(mesh, stack.outline, {}, input)
                       : geometry::apply_outline_cut_general(mesh, stack.outline, {}, input);
        check_result(result);
    }
}

void normalization_error(const InvalidRing& test) {
    const auto before = snapshot(test.ring);
    const auto result = geometry::normalize_ring(test.ring);
    require(snapshot(test.ring).words == before.words, "normalize_ring must preserve input bits");
    require(!result, "I12 must still reject a degenerate ring");
    exact_error(result.error(), "outline.unsupported", "outline must be a convex polygon");
}

void canonical_normalization(const Ring2& input, const Ring2& canonical) {
    const auto before = snapshot(input);
    const auto result = geometry::normalize_ring(input);
    require(snapshot(input).words == before.words, "normalize_ring must preserve input bits");
    require(static_cast<bool>(result), "I12 normalization must accept a simple polygon");
    require(snapshot(result.value()).words == snapshot(canonical).words,
            "normalized coordinates must match handwritten canonical CCW ring bit for bit");
}

void outline_error(const Ring2& ring) {
    auto stack = region_stack(simple_rings()[0].canonical);
    auto mesh = sp5::grid(stack, 8);
    sentinels(mesh);
    const auto before = snapshot(mesh);
    stack.outline.polygon = ring;
    // I12's nonconvex outline validation lives in the general entry, as in
    // test_outline_nonconvex_cut.cpp::bowtie_error. The public legacy overload
    // keeps its convex-only diagnostics.
    const auto result = geometry::apply_outline_cut_general(mesh, stack.outline, {}, {});
    require(snapshot(mesh).words == before.words, "failed outline must preserve every mesh field");
    require(!result, "I12 must reject a self-intersecting outline");
    exact_error(result.error(), "outline.unsupported", "self-intersecting outline");
}
} // namespace

int main() {
    const auto start = std::chrono::steady_clock::now();
    return sp5::run([&] {
        int cases = 0, failures = 0;
        const auto run = [&](const std::string& name, const auto& test) {
            ++cases;
            std::cout << "CASE " << name << std::endl;
            try {
                test();
                std::cout << "PASS " << name << '\n';
            } catch (const std::exception& error) {
                ++failures;
                std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            }
        };

        for (const auto& test : invalid_rings()) {
            run("region/" + test.name, [&] { region_error(test); });
            for (std::size_t entry = 0; entry < entries.size(); ++entry)
                run("consumer/" + std::string(entries[entry]) + "/" + test.name,
                    [&] { consumer_case(&test, entry); });
            if (test.name == "B0" || test.name == "H0" || test.name == "C1" || test.name == "C2" ||
                test.name == "tiny-square" || test.name == "nan-square" ||
                test.name == "inf-square")
                run("I12/normalize-reject/" + test.name, [&] { normalization_error(test); });
            if (test.name == "B0" || test.name == "H0")
                run("I12/outline-reject/" + test.name, [&] { outline_error(test.ring); });
        }

        for (const auto& test : simple_rings()) {
            run("region/S-" + test.name, [&] {
                const auto result =
                    region_cut_planes(region_stack(test.canonical), sp5b::registered_mesh());
                require(static_cast<bool>(result), "simple region must succeed");
            });
            run("I12/normalize/S-" + test.name,
                [&] { canonical_normalization(test.canonical, test.canonical); });
            run("I12/normalize/reversed-rotated-closed/S-" + test.name, [&] {
                auto input = test.canonical;
                std::reverse(input.begin(), input.end());
                std::rotate(input.begin(), input.begin() + 1, input.end());
                input.push_back(input.front());
                canonical_normalization(input, test.canonical);
            });
        }
        for (std::size_t entry = 0; entry < entries.size(); ++entry)
            run("consumer/" + std::string(entries[entry]) + "/valid-square",
                [&] { consumer_case(nullptr, entry); });

        // Sample once after all geometry cases, even when classification is red.
        run("measurement", [&] {
            rusage usage{};
            require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage must succeed");
            const double wall_s =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
#if defined(__APPLE__)
            const double rss_mib = static_cast<double>(usage.ru_maxrss) / 1048576.;
#else
            const double rss_mib = static_cast<double>(usage.ru_maxrss) / 1024.;
#endif
            std::printf("MEASURE wall_s=%.3f rss_mib=%.1f\n", wall_s, rss_mib);
            require(wall_s <= 5, "wall time must be at most 5 seconds");
            require(rss_mib <= 256, "peak RSS must be at most 256 MiB");
        });
        std::cout << "RESULT cases=" << cases << " passed=" << cases - failures
                  << " failed=" << failures << '\n';
        require(failures == 0, "acceptance cases failed; see named failures above");
    });
}
