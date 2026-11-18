#include "chipcae/mesh/testing/mesh_fixture.hpp"

#include "../../../preview/src/mesh_digest.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <jsoncons/json.hpp>
#include <jsoncons/json_cursor.hpp>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace chipcae::mesh::testing {
namespace {
using J = jsoncons::json;
void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
template <class F>
void fields(AttributeRecord& x, F&& f) {
    f("layer", x.layer);
    f("region", x.region);
    f("material", x.material);
    f("body", x.body);
}
template <class F>
void fields(SnapReport& x, F&& f) {
    f("axis", x.axis);
    f("from", x.from);
    f("to", x.to);
    f("displacement", x.displacement);
}
template <class F>
void fields(HexMeshArrays::InterfaceReport& x, F&& f) {
    f("name", x.name);
    f("side_a_bodies", x.side_a_bodies);
    f("side_b_bodies", x.side_b_bodies);
    f("aligned_faces", x.aligned_faces);
    f("cut_faces", x.cut_faces);
    f("aligned_area", x.aligned_area);
    f("cut_area", x.cut_area);
}
template <class F>
void fields(HexMeshArrays::GridReport& x, F&& f) {
    f("levels", x.levels);
    f("rule", x.rule);
    f("N_L", x.N_L);
    f("auto_selected", x.auto_selected);
    f("padding", x.padding);
    f("max_aspect_ratio", x.max_aspect_ratio);
    f("level_axes", x.level_axes);
    f("column_ranks", x.column_ranks);
    f("warnings", x.warnings);
    f("partition_imbalance", x.partition_imbalance);
}
template <class F>
void fields(AttributeTable& x, F&& f) {
    f("layers", x.layers);
    f("entries", x.entries);
    f("references", x.references);
    f("materials", x.materials);
    f("bodies", x.bodies);
    f("boundary_names", x.boundary_names);
}
template <class F>
void fields(HexMeshArrays& x, F&& f) {
    f("vertices", x.vertices);
    f("hex", x.hex);
    f("attribute", x.attribute);
    f("bdr_quad", x.bdr_quad);
    f("bdr_attribute", x.bdr_attribute);
    f("elem_gid", x.elem_gid);
    f("node_gid", x.node_gid);
    f("partitioning", x.partitioning);
    f("node_layer", x.node_layer);
    f("copper_layer_of_layer", x.copper_layer_of_layer);
    f("copper_phi", x.copper_phi);
    f("node_column", x.node_column);
    f("attr_frame", x.attr_frame);
    f("nodes", x.nodes);
    f("internal_interfaces", x.internal_interfaces);
    f("order", x.order);
    f("preparation_report", x.preparation_report);
    f("grid_report", x.grid_report);
    f("attr_table", x.attr_table);
    f("face_sets", x.face_sets);
    f("pending_cut", x.pending_cut);
    f("snapped", x.snapped);
    f("axes", x.axes);
}

template <class T>
void decode(const J& j, T& x);
struct Reader {
    const J& j;
    std::size_t count = 0;
    template <class T>
    void operator()(const char* name, T& x) {
        check(j.contains(name), "missing required field");
        decode(j.at(name), x);
        ++count;
    }
};
template <class T>
void decode(const J& j, T& x) {
    if constexpr (std::is_same_v<T, bool>) {
        check(j.is_bool(), "expected boolean");
        x = j.as<bool>();
    } else if constexpr (std::is_same_v<T, char>) {
        check(j.is_string() && j.as<std::string>().size() == 1, "expected single character");
        x = j.as<std::string>()[0];
    } else if constexpr (std::is_integral_v<T>) {
        check(j.type() == jsoncons::json_type::int64_value ||
                  j.type() == jsoncons::json_type::uint64_value,
              "expected integer");
        if (j.type() == jsoncons::json_type::int64_value) {
            auto v = j.as<int64_t>();
            if constexpr (std::is_signed_v<T>)
                check(v >= std::numeric_limits<T>::min() && v <= std::numeric_limits<T>::max(),
                      "integer range");
            else
                check(v >= 0 && static_cast<uint64_t>(v) <= std::numeric_limits<T>::max(),
                      "unsigned range");
            x = static_cast<T>(v);
        } else {
            auto v = j.as<uint64_t>();
            check(v <= static_cast<uint64_t>(std::numeric_limits<T>::max()), "integer range");
            x = static_cast<T>(v);
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        check(j.type() == jsoncons::json_type::double_value ||
                  j.type() == jsoncons::json_type::int64_value ||
                  j.type() == jsoncons::json_type::uint64_value,
              "expected real");
        x = j.as<T>();
        check(std::isfinite(x), "nonfinite real");
    } else if constexpr (std::is_same_v<T, std::string>) {
        check(j.is_string(), "expected string");
        x = j.as<std::string>();
    } else if constexpr (requires { typename T::mapped_type; }) {
        check(j.is_array(), "expected map pairs");
        for (const auto& pair : j.array_range()) {
            check(pair.is_array() && pair.size() == 2, "expected map pair");
            typename T::key_type k;
            typename T::mapped_type v;
            decode(pair[0], k);
            decode(pair[1], v);
            check(x.emplace(std::move(k), std::move(v)).second, "duplicate map key");
        }
    } else if constexpr (requires {
                             x.begin();
                             x.end();
                         }) {
        check(j.is_array(), "expected array");
        if constexpr (requires { x.resize(j.size()); })
            x.resize(j.size());
        else
            check(j.size() == x.size(), "array length");
        std::size_t i = 0;
        for (auto& v : x)
            decode(j[i++], v);
    } else {
        check(j.is_object(), "expected object");
        Reader reader{j};
        fields(x, reader);
        check(reader.count == j.size(), "unknown field");
    }
}
// Emit directly so floating point spelling is independent of JSON library defaults.
template <class T>
void encode(std::string& out, const T& x);
struct Writer {
    std::string& out;
    bool first = true;
    template <class T>
    void operator()(const char* name, const T& x) {
        if (!first)
            out += ',';
        first = false;
        encode(out, std::string(name));
        out += ':';
        encode(out, x);
    }
};
template <class T>
void encode(std::string& out, const T& x) {
    if constexpr (std::is_same_v<T, bool>)
        out += x ? "true" : "false";
    else if constexpr (std::is_same_v<T, char>)
        encode(out, std::string(1, x));
    else if constexpr (std::is_arithmetic_v<T>) {
        char buffer[128];
        auto result = [&] {
            if constexpr (std::is_floating_point_v<T>) {
                check(std::isfinite(x), "nonfinite real");
                return std::to_chars(
                    buffer, buffer + sizeof(buffer), x, std::chars_format::general, 17);
            } else
                return std::to_chars(buffer, buffer + sizeof(buffer), x);
        }();
        check(result.ec == std::errc{}, "number encoding");
        std::string number(buffer, result.ptr);
        if constexpr (std::is_floating_point_v<T>)
            if (number.find_first_of(".eE") == std::string::npos)
                number += ".0";
        out += number;
    } else if constexpr (std::is_same_v<T, std::string>)
        out += J(x).to_string();
    else if constexpr (requires { typename T::mapped_type; }) {
        out += '[';
        bool first = true;
        // All public maps are std::map: iteration explicitly follows sorted keys.
        for (const auto& [k, v] : x) {
            if (!first)
                out += ',';
            first = false;
            out += '[';
            encode(out, k);
            out += ',';
            encode(out, v);
            out += ']';
        }
        out += ']';
    } else if constexpr (requires {
                             x.begin();
                             x.end();
                         }) {
        out += '[';
        bool first = true;
        for (const auto& v : x) {
            if (!first)
                out += ',';
            first = false;
            encode(out, v);
        }
        out += ']';
    } else {
        out += '{';
        Writer writer{out};
        // fields only describes members; encoding never modifies them.
        fields(const_cast<T&>(x), writer);
        out += '}';
    }
}
void reject_duplicate_fields(const std::string& text) {
    jsoncons::json_string_cursor cursor(text);
    std::vector<std::set<std::string>> objects;
    for (; !cursor.done(); cursor.next()) {
        const auto& e = cursor.current();
        if (e.event_type() == jsoncons::staj_event_type::begin_object)
            objects.emplace_back();
        else if (e.event_type() == jsoncons::staj_event_type::end_object) {
            check(!objects.empty(), "unexpected object end");
            objects.pop_back();
        } else if (e.event_type() == jsoncons::staj_event_type::key) {
            check(!objects.empty(), "object field outside object");
            check(objects.back().insert(e.get<std::string>()).second, "duplicate object field");
        }
    }
}
bool validate(const HexMeshArrays& m) {
    auto n = m.node_gid.size(), c = m.elem_gid.size();
    check(m.vertices.size() / 3 == n && m.vertices.size() % 3 == 0, "vertices length");
    check(m.hex.size() / 8 == c && m.hex.size() % 8 == 0, "hex length");
    check(m.attribute.size() == c, "attribute length");
    for (auto attribute : m.attribute)
        check(attribute >= 1 && static_cast<std::size_t>(attribute) <= m.attr_table.entries.size(),
              "attribute table reference");
    for (auto attribute : m.bdr_attribute)
        check(m.attr_table.boundary_names.contains(attribute), "boundary name reference");
    check(m.bdr_quad.size() % 4 == 0 && m.bdr_quad.size() / 4 == m.bdr_attribute.size(),
          "boundary length");
    for (auto i : m.hex)
        check(i >= 0 && static_cast<std::size_t>(i) < n, "hex connectivity");
    for (auto i : m.bdr_quad)
        check(i >= 0 && static_cast<std::size_t>(i) < n, "boundary connectivity");
    for (auto i : m.pending_cut)
        check(i >= 0 && static_cast<std::size_t>(i) < c, "pending cut index");
    // Reserved curved-node storage contains 3D coordinates, independently of
    // the linear vertex count (higher orders can introduce additional nodes).
    check(m.nodes.size() % 3 == 0, "nodes coordinate length");
    for (auto x : m.nodes)
        check(std::isfinite(x), "nonfinite node coordinate");
    for (const auto& snap : m.snapped) {
        check(snap.axis == 'x' || snap.axis == 'y' || snap.axis == 'z', "snap axis");
        check(std::isfinite(snap.from) && std::isfinite(snap.to) &&
                  std::isfinite(snap.displacement),
              "nonfinite snap report");
    }
    for (const auto& [name, attributes] : m.face_sets) {
        check(!name.empty(), "empty face set name");
        for (auto i : attributes)
            check(i >= 0 && static_cast<std::size_t>(i) < m.bdr_quad.size() / 4, "face set index");
    }
    auto optional = [](std::size_t actual, std::size_t expected) {
        check(actual == 0 || actual == expected, "optional array length");
    };
    optional(m.partitioning.size(), c);
    optional(m.node_layer.size(), n);
    optional(m.node_column.size(), n);
    check(m.attr_frame.size() % 9 == 0, "attribute frame length");
    if (n == 0 && c == 0) {
        std::string actual, defaults;
        encode(actual, m);
        encode(defaults, HexMeshArrays{});
        check(actual == defaults, "only default empty mesh permits null digest");
        return true;
    }
    check(n > 0 && c > 0, "incomplete mesh");
    check(m.grid_report.levels > 0 &&
              m.grid_report.level_axes.size() == static_cast<std::size_t>(m.grid_report.levels),
          "level axes shape");
    auto axis = [](const auto& a) {
        check(a.size() >= 2, "missing axis");
        for (std::size_t i = 0; i < a.size(); ++i) {
            check(std::isfinite(a[i]), "nonfinite axis");
            if (i)
                check(a[i] > a[i - 1], "axis ordering");
        }
    };
    check(m.axes.size() == 3, "axes shape");
    for (const auto& a : m.axes)
        axis(a);
    for (const auto& level : m.grid_report.level_axes) {
        check(level.size() == 2, "level axes count");
        for (const auto& a : level)
            axis(a);
    }
    optional(m.grid_report.max_aspect_ratio.size(), m.grid_report.level_axes.size());
    optional(m.grid_report.column_ranks.size(), m.grid_report.level_axes.size());
    for (std::size_t i = 0; i < m.grid_report.column_ranks.size(); ++i) {
        const auto& a = m.grid_report.level_axes[i];
        check(m.grid_report.column_ranks[i].size() == (a[0].size() - 1) * (a[1].size() - 1),
              "column ranks shape");
    }
    optional(m.copper_layer_of_layer.size(), m.attr_table.layers.size());
    auto columns = (m.axes[0].size() - 1) * (m.axes[1].size() - 1);
    std::size_t copper = 0;
    for (auto i : m.copper_layer_of_layer) {
        check(i >= -1 && (i == -1 || static_cast<std::size_t>(i) < m.attr_table.layers.size()),
              "copper layer index");
        if (i >= 0)
            copper = std::max(copper, static_cast<std::size_t>(i) + 1);
    }
    optional(m.copper_phi.size(), copper * columns);
    return false;
}
} // namespace
Expected<void> save_mesh_fixture(const HexMeshArrays& mesh, const std::filesystem::path& path) {
    std::string text;
    try {
        const bool empty = validate(mesh);
        text = "{\"mesh\":";
        encode(text, mesh);
        text += ",\"mesh_sha256\":";
        if (empty)
            text += "null";
        else
            encode(text, chipcae::preview::detail::digest(mesh).sha256);
        text += "}\n";
    } catch (const std::exception& e) {
        return Error{"INPUT_INVALID", e.what()};
    }
    try {
        std::ofstream out;
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.open(path, std::ios::binary | std::ios::trunc);
        out << text;
        out.close();
    } catch (const std::exception& e) {
        return Error{"$", e.what()};
    }
    return {};
}
Expected<HexMeshArrays> load_mesh_fixture(const std::filesystem::path& path) {
    try {
        std::ifstream in(path, std::ios::binary);
        check(bool(in), "cannot read mesh fixture");
        std::string text{std::istreambuf_iterator<char>(in), {}};
        check(!in.bad(), "cannot read mesh fixture");
        reject_duplicate_fields(text);
        jsoncons::json_options options;
        options.lossless_bignum(true);
        auto j = J::parse(text, options);
        check(j.is_object() && j.size() == 2 && j.contains("mesh") && j.contains("mesh_sha256"),
              "fixture fields");
        HexMeshArrays mesh;
        decode(j.at("mesh"), mesh);
        const bool empty = validate(mesh);
        const auto& hash = j.at("mesh_sha256");
        if (empty)
            check(hash.is_null(), "empty digest must be null");
        else {
            check(hash.is_string(), "digest must be string");
            auto s = hash.as<std::string>();
            check(s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos,
                  "digest format");
            check(s == chipcae::preview::detail::digest(mesh).sha256, "digest mismatch");
        }
        return mesh;
    } catch (const std::exception& e) {
        return Error{"INPUT_INVALID", e.what()};
    }
}
} // namespace chipcae::mesh::testing
