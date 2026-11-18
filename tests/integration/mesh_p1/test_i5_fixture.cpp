#include "../../../preview/src/mesh_digest.hpp"
#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/testing/mesh_fixture.hpp"

#include <bit>
#include <chrono>
#include <fstream>
#include <iostream>
#include <jsoncons/json.hpp>
#include <limits>
#include <stdexcept>
#include <type_traits>

using namespace chipcae;
using namespace chipcae::mesh;
using namespace chipcae::mesh::testing;
using J = jsoncons::json;
namespace fs = std::filesystem;
namespace {
void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::string bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    require(bool(in), "read " + p.string());
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
    require(bool(out), "write " + p.string());
}
// Independent test projection: bit mode compares every Real by its IEEE bits;
// numeric mode supplies input documents without depending on production save.
struct Projection {
    bool bits = false;
    J value(Real x) const { return bits ? J(std::bit_cast<std::uint64_t>(x)) : J(x); }
    template <class T>
    J value(const T& x) const {
        return J(x);
    }
    template <class T>
    J value(const std::vector<T>& x) const {
        J a = J::array();
        for (const auto& v : x)
            a.push_back(value(v));
        return a;
    }
    template <class T, std::size_t N>
    J value(const std::array<T, N>& x) const {
        J a = J::array();
        for (const auto& v : x)
            a.push_back(value(v));
        return a;
    }
    template <class K, class V>
    J value(const std::map<K, V>& x) const {
        J a = J::array();
        for (const auto& [k, v] : x) {
            J pair = J::array();
            pair.push_back(value(k));
            pair.push_back(value(v));
            a.push_back(pair);
        }
        return a;
    }
    J value(const AttributeRecord& x) const {
        J j;
        j["layer"] = x.layer;
        j["region"] = x.region;
        j["material"] = x.material;
        j["body"] = x.body;
        return j;
    }
    J value(const SnapReport& x) const {
        J j;
        j["axis"] = std::string(1, x.axis);
        j["from"] = value(x.from);
        j["to"] = value(x.to);
        j["displacement"] = value(x.displacement);
        return j;
    }
    J value(const HexMeshArrays::InterfaceReport& x) const {
        J j;
#define FIELD(name) j[#name] = value(x.name)
        FIELD(name);
        FIELD(side_a_bodies);
        FIELD(side_b_bodies);
        FIELD(aligned_faces);
        FIELD(cut_faces);
        FIELD(aligned_area);
        FIELD(cut_area);
        return j;
    }
    J value(const HexMeshArrays::GridReport& x) const {
        J j;
        FIELD(levels);
        FIELD(rule);
        FIELD(N_L);
        FIELD(auto_selected);
        FIELD(padding);
        FIELD(max_aspect_ratio);
        FIELD(level_axes);
        FIELD(column_ranks);
        FIELD(warnings);
        FIELD(partition_imbalance);
        return j;
    }
    J value(const AttributeTable& x) const {
        J j;
        FIELD(layers);
        FIELD(entries);
        FIELD(references);
        FIELD(materials);
        FIELD(bodies);
        FIELD(boundary_names);
        return j;
    }
    J value(const HexMeshArrays& x) const {
        J j;
        FIELD(vertices);
        FIELD(hex);
        FIELD(attribute);
        FIELD(bdr_quad);
        FIELD(bdr_attribute);
        FIELD(elem_gid);
        FIELD(node_gid);
        FIELD(partitioning);
        FIELD(node_layer);
        FIELD(copper_layer_of_layer);
        FIELD(copper_phi);
        FIELD(node_column);
        FIELD(attr_frame);
        FIELD(nodes);
        FIELD(internal_interfaces);
        FIELD(order);
        FIELD(preparation_report);
        FIELD(grid_report);
        FIELD(attr_table);
        FIELD(face_sets);
        FIELD(pending_cut);
        FIELD(snapped);
        FIELD(axes);
        return j;
#undef FIELD
    }
};
J document(const HexMeshArrays& m) {
    J j;
    j["mesh"] = Projection{}.value(m);
    j["mesh_sha256"] = m.hex.empty() ? J::null() : J(preview::detail::digest(m).sha256);
    return j;
}
void equal(const HexMeshArrays& a, const HexMeshArrays& b) {
    auto left = Projection{true}.value(a), right = Projection{true}.value(b);
    for (const auto& field : left.object_range())
        require(field.value() == right.at(field.key()),
                "bit-exact field equality: " + std::string(field.key()));
}
HexMeshArrays generated() {
    const fs::path root(I5_ROOT);
    auto stack = load_stack(root / "schema/examples/fcbga_demo.stack.json",
                            root / "schema/stack-0.2.schema.json");
    require(bool(stack), "demo stack loads");
    GridOptions opt;
    opt.nx = 2;
    opt.ny = 2;
    opt.ranks = 1;
    opt.levels = 1;
    opt.max_ratio = 16; // The unchanged demo has a 10:1 thickness jump.
    auto mesh = build_tensor_grid(stack.value(), opt);
    require(bool(mesh),
            mesh ? ""
                 : "existing build_tensor_grid: " + mesh.error().path + ": " + mesh.error().reason);
    return mesh.value();
}
HexMeshArrays decorated() {
    // Hand-built region/void stub: dense column 0 is absent, column 1
    // contains one hex. This models an input shape, never a real cut result.
    HexMeshArrays m;
    m.vertices = {1, 0, -0.0, 2, 0, 0, 2, 1, 0, 1, 1, 0, 1, 0, 1, 2, 0, 1, 2, 1, 1, 1, 1, 1};
    m.hex = {0, 1, 2, 3, 4, 5, 6, 7};
    m.attribute = {2};
    m.elem_gid = {1};
    m.node_gid = {0, 1, 2, 3, 4, 5, 6, 7};
    m.partitioning = {0};
    m.node_layer = {0, 0, 0, 0, 0, 0, 0, 0};
    m.node_column = {1, 2, 5, 4, 1, 2, 5, 4};
    m.copper_layer_of_layer = {0, -1};
    m.copper_phi = {0.25, 0.75};
    m.axes = {{{0, 1, 2}, {0, 1}, {0, 1}}};
    m.grid_report.level_axes = {{{m.axes[0], m.axes[1]}}};
    m.grid_report.column_ranks = {{0, 0}};
    m.grid_report.N_L = 2;
    m.grid_report.max_aspect_ratio = {1};
    m.bdr_quad = {0, 1, 2, 3, 4, 5, 6, 7};
    m.bdr_attribute = {1, 2};
    m.attr_table.entries = {{"Case", "", 17, 23}, {"case", "region", 18, 24}};
    m.attr_frame = {1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    m.snapped = {{'x', -0.0, 0.125, 0.125}, {'y', 0.3, 0.30000000000000004, 5.551115123125783e-17}};
    m.preparation_report = {
        {"Interface", {"BodyA", "bodya"}, {"BodyB"}, 9007199254740993LL, 7, -0.0, 0.3}};
    m.grid_report.warnings = {"Case", "case"};
    m.grid_report.partition_imbalance = 1.0000000000000002;
    m.grid_report.auto_selected = true;
    m.grid_report.rule = 1;
    m.internal_interfaces = true;
    m.attr_table.layers["Case"] = 1;
    m.attr_table.layers["case"] = 2;

    m.attr_table.references = {{"Case", {1, 2}}, {"case", {2, 1}}};
    m.attr_table.materials = {{"Case", 17}, {"case", 18}};
    m.attr_table.bodies = {{"BodyA", 23}, {"BodyB", 24}};
    m.attr_table.boundary_names = {{1, "Case"}, {2147483647, "case"}};
    m.face_sets = {{"Case", {1, 2}}, {"case", {2, 1}}};
    m.pending_cut = {1};
    // Large global IDs are opaque identifiers; preserve their exact integer type.
    m.node_gid.back() = 9007199254740993LL;
    return m;
}
void roundtrip(const HexMeshArrays& m, const fs::path& p) {
    static_assert(std::is_same_v<decltype(save_mesh_fixture(m, p)), Expected<void>>);
    auto saved = save_mesh_fixture(m, p);
    require(bool(saved), saved ? "" : "save succeeds: " + saved.error().reason);
    auto loaded = load_mesh_fixture(p);
    require(bool(loaded), loaded ? "" : "load succeeds: " + loaded.error().reason);
    equal(m, loaded.value());
}
void rejects_bad(const fs::path& dir) {
    int failures = 0;
    auto reject = [&](const std::string& label, const std::string& text, bool missing = false) {
        const auto p = dir / "bad.json";
        if (missing)
            fs::remove(p);
        else
            write(p, text);
        try {
            auto r = load_mesh_fixture(p);
            if (r || r.error().path != "INPUT_INVALID") {
                std::cerr << "FAIL " << label << ": expected INPUT_INVALID";
                if (!r)
                    std::cerr << ", got " << r.error().path << ": " << r.error().reason;
                std::cerr << '\n';
                ++failures;
            }
        } catch (const std::exception& e) {
            std::cerr << "FAIL " << label << ": threw " << e.what() << '\n';
            ++failures;
        }
    };
    const auto base = document(generated());
    // Positive control prevents a loader that rejects everything from satisfying the test.
    write(dir / "valid.json", base.to_string());
    auto valid = load_mesh_fixture(dir / "valid.json");
    if (!valid) {
        std::cerr << "FAIL positive control: " << valid.error().reason << '\n';
        ++failures;
    }
    reject("missing file", "", true);
    reject("non JSON", "{broken");
    auto mutate = [&](const std::string& label, auto edit) {
        auto j = base;
        edit(j);
        reject(label, j.to_string());
    };
    for (const auto& field : base.at("mesh").object_range()) {
        const std::string key(field.key());
        mutate("missing mesh." + key, [&](J& j) { j["mesh"].erase(key); });
        mutate("wrong type mesh." + key, [&](J& j) { j["mesh"][key] = J::null(); });
    }
    for (const std::string nested : {"grid_report", "attr_table"})
        for (const auto& field : base.at("mesh").at(nested).object_range()) {
            const std::string key(field.key());
            mutate("missing " + nested + "." + key, [&](J& j) { j["mesh"][nested].erase(key); });
        }
    mutate("missing mesh", [](J& j) { j.erase("mesh"); });
    mutate("missing digest", [](J& j) { j.erase("mesh_sha256"); });
    mutate("unknown top", [](J& j) { j["extra"] = 0; });
    mutate("unknown mesh", [](J& j) { j["mesh"]["extra"] = 0; });
    mutate("wrong casing", [](J& j) {
        j["mesh"]["Vertices"] = j["mesh"]["vertices"];
        j["mesh"].erase("vertices");
    });
    mutate("unknown nested", [](J& j) { j["mesh"]["grid_report"]["Levels"] = 1; });
    for (const std::string field :
         {"hex", "attribute", "bdr_quad", "bdr_attribute", "node_gid", "elem_gid", "vertices"}) {
        mutate("invalid length " + field, [&](J& j) { j["mesh"][field].push_back(0); });
    }
    for (const std::string field :
         {"partitioning", "node_layer", "node_column", "attr_frame", "copper_phi"}) {
        mutate("nonempty optional shape " + field, [&](J& j) { j["mesh"][field].push_back(0); });
    }
    mutate("wrong integer element type", [](J& j) { j["mesh"]["hex"][0] = "0"; });
    mutate("wrong real element type", [](J& j) { j["mesh"]["vertices"][0] = "0"; });
    mutate("wrong bool type", [](J& j) { j["mesh"]["internal_interfaces"] = 1; });
    mutate("wrong report type", [](J& j) { j["mesh"]["grid_report"]["auto_selected"] = "true"; });
    mutate("connectivity upper bound", [](J& j) { j["mesh"]["hex"][0] = 2147483647; });
    mutate("negative connectivity", [](J& j) { j["mesh"]["hex"][0] = -1; });
    mutate("boundary connectivity", [](J& j) { j["mesh"]["bdr_quad"][0] = 2147483647; });
    for (const std::string token : {"2147483648", "-2147483649", "1.0", "true", "\"1\""}) {
        auto s = base.to_string();
        const auto pos = s.find("\"order\":");
        const auto start = pos + 8;
        const auto end = s.find_first_of(",}", start);
        s.replace(start, end - start, token);
        reject("int32 type/range " + token, s);
    }
    for (const std::string token :
         {"9223372036854775808", "-9223372036854775809", "18446744073709551616"}) {
        auto s = base.to_string();
        const auto start = s.find("\"node_gid\":[") + 12;
        const auto end = s.find_first_of(",]", start);
        s.replace(start, end - start, token);
        reject("int64 range " + token, s);
    }
    mutate("size underflow", [](J& j) { j["mesh"]["grid_report"]["N_L"] = -1; });
    mutate("too few axes", [](J& j) { j["mesh"]["axes"] = J::array(); });
    mutate("too many axes", [](J& j) { j["mesh"]["axes"].push_back(J::array()); });
    mutate("missing digest axes", [](J& j) { j["mesh"]["axes"][2] = J::array(); });
    mutate("missing level axes", [](J& j) { j["mesh"]["grid_report"]["level_axes"] = J::array(); });
    mutate("malformed digest", [](J& j) { j["mesh_sha256"] = "ABC"; });
    mutate("uppercase digest", [](J& j) { j["mesh_sha256"] = std::string(64, 'A'); });
    mutate("mismatched digest", [](J& j) { j["mesh_sha256"] = std::string(64, '0'); });
    mutate("nonempty null digest", [](J& j) { j["mesh_sha256"] = J::null(); });
    mutate("duplicate integer map key", [](J& j) {
        j["mesh"]["attr_table"]["boundary_names"] = J::parse("[[1,\"a\"],[1,\"b\"]]");
    });
    mutate("duplicate string map key",
           [](J& j) { j["mesh"]["face_sets"] = J::parse("[[\"a\",[]],[\"a\",[]]]"); });
    mutate("integer map key type",
           [](J& j) { j["mesh"]["attr_table"]["boundary_names"] = J::parse("[[\"1\",\"a\"]]"); });
    auto duplicate = base.to_string();
    duplicate.insert(1, "\"mesh_sha256\":null,");
    reject("duplicate object field", duplicate);
    duplicate = base.to_string();
    duplicate.insert(duplicate.find("\"order\":"), "\"order\":1,");
    reject("duplicate nested field", duplicate);
    for (const std::string token : {"NaN", "Infinity", "-Infinity", "1e999"}) {
        auto s = base.to_string();
        auto start = s.find("\"vertices\":[") + 12;
        auto end = s.find_first_of(",]", start);
        s.replace(start, end - start, token);
        reject("nonfinite " + token, s);
    }
    for (const Real invalid : {std::numeric_limits<Real>::infinity(),
                               -std::numeric_limits<Real>::infinity(),
                               std::numeric_limits<Real>::quiet_NaN()}) {
        auto m = generated();
        m.vertices[0] = invalid;
        auto r = save_mesh_fixture(m, dir / "nonfinite.json");
        if (r || r.error().path != "INPUT_INVALID") {
            std::cerr << "FAIL save nonfinite: expected INPUT_INVALID\n";
            ++failures;
        }
    }
    require(failures == 0,
            std::to_string(failures) +
                " independent invalid-input/positive-control assertions failed");
}
} // namespace
int main(int argc, char** argv) {
    fs::path dir;
    try {
        require(argc == 2, "one test case required");
        const std::string test(argv[1]);
        dir = fs::temp_directory_path() /
              ("i5-fixture-" + test + "-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(dir);
        const auto p = dir / "mesh.json";
        if (test == "generate-reference") {
            write(fs::path(I5_ROOT) / "tests/fixtures/mesh/i5_fcbga_small.mesh.json",
                  document(generated()).to_string() + "\n");
        } else if (test == "roundtrip")
            roundtrip(generated(), p);
        else if (test == "empty") {
            roundtrip(HexMeshArrays{}, p);
            require(J::parse(bytes(p)).at("mesh_sha256").is_null(), "empty mesh has null digest");
        } else if (test == "digest") {
            const auto fixture = fs::path(I5_ROOT) / "tests/fixtures/mesh/i5_fcbga_small.mesh.json";
            auto m = load_mesh_fixture(fixture);
            require(bool(m), m ? "" : "load checked-in fixture: " + m.error().reason);
            const auto recorded = J::parse(bytes(fixture)).at("mesh_sha256").as<std::string>();
            require(recorded == preview::detail::digest(m.value()).sha256,
                    "recorded digest equals recomputed digest");
        } else if (test == "deterministic") {
            const auto m = decorated();
            roundtrip(m, p);
            const auto first = bytes(p);
            roundtrip(m, p);
            require(first == bytes(p), "same-path saves produce identical bytes");
            auto extra_frame = m;
            extra_frame.attr_frame.insert(extra_frame.attr_frame.end(), 9, 0.0);
            roundtrip(extra_frame, dir / "extra-frame.json");
            auto json = J::parse(first);
            require(json["mesh"]["attr_table"]["boundary_names"][0][0].is_int64() ||
                        json["mesh"]["attr_table"]["boundary_names"][0][0].is_uint64(),
                    "boundary map keys remain integers");
            const auto check_sorted = [&](const J& pairs) {
                for (std::size_t i = 1; i < pairs.size(); ++i)
                    require(pairs[i - 1][0] < pairs[i][0],
                            "map pair arrays are explicitly key sorted");
            };
            check_sorted(json["mesh"]["face_sets"]);
            for (const std::string field :
                 {"layers", "references", "materials", "bodies", "boundary_names"})
                check_sorted(json["mesh"]["attr_table"][field]);
            require(json["mesh"]["node_gid"].as<std::vector<Index>>().back() == 9007199254740993LL,
                    "large integers are exact");
        } else if (test == "rejects_bad")
            rejects_bad(dir);
        else
            require(false, "unknown test case");
        fs::remove_all(dir);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        if (!dir.empty())
            fs::remove_all(dir);
        return 1;
    }
}
