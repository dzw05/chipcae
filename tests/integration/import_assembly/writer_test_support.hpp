#pragma once
#include "chipcae/schema/stack.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

// The importer is an executable, not a linkable library. Compile its private writer
// into this test TU. The fallback is test-local and deliberately returns an error.
#if __has_include("../../../kernel/src/stack_writer.cpp")
#include "../../../kernel/src/stack_writer.cpp"
#else
namespace chipcae::importer {
inline chipcae::mesh::Expected<bool> write_stack(const chipcae::mesh::StackModel&,
                                                 const std::filesystem::path&) {
    return chipcae::mesh::Error{"$", "SP4 write_stack is not implemented"};
}
} // namespace chipcae::importer
#endif

#if __has_include(<jsoncons/json.hpp>)
#include "../../../schema/src/stack.cpp"
#define SP4_HAS_JSONCONS 1
#endif

namespace sp4_test {
inline void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
inline std::string bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
inline void real(double a, double b) {
    require(std::abs(a - b) <= (a == 0 ? 1e-300 : std::abs(a) * 1e-15),
            "real field changed in roundtrip");
}
inline void footprint(const chipcae::mesh::Footprint& a, const chipcae::mesh::Footprint& b) {
    for (std::size_t i = 0; i < 4; ++i)
        real(a.box[i], b.box[i]);
    require(a.polygon.size() == b.polygon.size(), "polygon point count changed");
    for (std::size_t i = 0; i < a.polygon.size(); ++i)
        for (std::size_t j = 0; j < 2; ++j)
            real(a.polygon[i][j], b.polygon[i][j]);
}
inline void equal(const chipcae::mesh::StackModel& a, const chipcae::mesh::StackModel& b) {
    require(a.id == b.id && a.unit == b.unit && a.symmetry == b.symmetry &&
                a.package_type == b.package_type && a.warnings == b.warnings &&
                a.activation_groups == b.activation_groups,
            "stack metadata changed");
    footprint(a.outline, b.outline);
    require(a.layers.size() == b.layers.size(), "layer count changed");
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        const auto& x = a.layers[i];
        const auto& y = b.layers[i];
        require(x.name == y.name && x.kind == y.kind && x.material == y.material &&
                    x.body == y.body && x.n_elem_min == y.n_elem_min,
                "layer fields changed");
        real(x.thickness, y.thickness);
        real(x.grading, y.grading);
        require(x.regions.size() == y.regions.size(), "region count changed");
        for (std::size_t r = 0; r < x.regions.size(); ++r) {
            const auto& p = x.regions[r];
            const auto& q = y.regions[r];
            require(p.name == q.name && p.material == q.material && p.body == q.body,
                    "region metadata changed");
            footprint(p.footprint, q.footprint);
        }
        require(x.copper.has_value() == y.copper.has_value(), "copper presence changed");
        if (x.copper) {
            require(x.copper->metal == y.copper->metal &&
                        x.copper->fraction.has_value() == y.copper->fraction.has_value() &&
                        x.copper->polygons.has_value() == y.copper->polygons.has_value(),
                    "copper fields changed");
            if (x.copper->fraction)
                real(*x.copper->fraction, *y.copper->fraction);
            if (x.copper->polygons)
                require(x.copper->polygons->file == y.copper->polygons->file &&
                            x.copper->polygons->layer == y.copper->polygons->layer,
                        "copper path or layer changed");
        }
    }
}
} // namespace sp4_test
