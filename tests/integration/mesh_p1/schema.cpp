#include "../../../schema/src/sha256.hpp"
#include "chipcae/schema/stack.hpp"

#include <fstream>
#include <iostream>
#include <jsoncons/json.hpp>
#include <stdexcept>
using namespace chipcae::mesh;
void expect_impl(bool ok, int line) {
    if (!ok)
        throw std::runtime_error("schema expectation failed at line " + std::to_string(line));
}
#define expect(ok) expect_impl((ok), __LINE__)
int main() {
    auto root = std::filesystem::path(K1_ROOT);
    auto schema = root / "schema/stack-0.1.schema.json";
    auto schema02 = root / "schema/stack-0.2.schema.json";
    for (auto name : {"fcbga_demo", "interposer_2p5d"}) {
        auto r =
            load_stack(root / (std::string("schema/examples/") + name + ".stack.json"), schema);
        if (!r)
            std::cerr << r.error().path << ": " << r.error().reason << '\n';
        expect(bool(r));
        auto old_model =
            load_stack(root / (std::string("schema/examples/") + name + ".stack.json"), schema);
        auto new_model =
            load_stack(root / (std::string("schema/examples/") + name + ".stack.json"), schema02);
        expect(bool(new_model));
        expect(old_model.value().id == new_model.value().id);
        expect(old_model.value().unit == new_model.value().unit);
        expect(old_model.value().symmetry == new_model.value().symmetry);
        expect(old_model.value().package_type == new_model.value().package_type);
        expect(old_model.value().warnings == new_model.value().warnings);
        expect(old_model.value().outline.box == new_model.value().outline.box);
        expect(old_model.value().outline.polygon == new_model.value().outline.polygon);
        expect(old_model.value().activation_groups == new_model.value().activation_groups);
        expect(old_model.value().layers.size() == new_model.value().layers.size());
        for (std::size_t i = 0; i < old_model.value().layers.size(); ++i) {
            const auto& a = old_model.value().layers[i];
            const auto& b = new_model.value().layers[i];
            expect(a.name == b.name && a.kind == b.kind && a.material == b.material &&
                   a.body == b.body && a.thickness == b.thickness && a.grading == b.grading &&
                   a.n_elem_min == b.n_elem_min && a.regions.size() == b.regions.size() &&
                   !b.copper);
            for (std::size_t region = 0; region < a.regions.size(); ++region) {
                const auto& old_region = a.regions[region];
                const auto& new_region = b.regions[region];
                expect(old_region.name == new_region.name &&
                       old_region.material == new_region.material &&
                       old_region.body == new_region.body &&
                       old_region.footprint.box == new_region.footprint.box &&
                       old_region.footprint.polygon == new_region.footprint.polygon);
            }
        }
    }
    auto copper_example = root / "schema/examples/fcbga_cu.stack.json";
    auto copper_valid = load_stack(copper_example, schema02);
    if (!copper_valid)
        std::cerr << copper_valid.error().path << ": " << copper_valid.error().reason << '\n';
    expect(bool(copper_valid));
    expect(!copper_valid.value().layers.empty());
    expect(copper_valid.value().layers[0].copper.has_value());
    expect(copper_valid.value().layers[0].kind == "Patterned");
    expect(copper_valid.value().layers[0].material == "substrate_resin");
    expect(copper_valid.value().layers[0].copper->metal == "copper");
    expect(copper_valid.value().layers[0].copper->fraction == 0.32);
    std::ifstream copper_in(copper_example);
    auto copper_base = jsoncons::json::parse(copper_in);
    const auto copper_path = std::filesystem::current_path() / "copper-invalid.stack.json";
    auto expect_copper_error = [&](jsoncons::json invalid, const std::string& field) {
        std::ofstream out(copper_path);
        out << invalid;
        out.close();
        auto result = load_stack(copper_path, schema02);
        expect(!result && result.error().path.find(field) != std::string::npos);
    };
    auto invalid = copper_base;
    invalid["layers"][0]["copper"]["fraction"] = 1.2;
    expect_copper_error(invalid, "fraction");
    invalid = copper_base;
    invalid["layers"][0]["copper"]["polygons"] =
        jsoncons::json::parse(R"({"file":"poly.bin","layer":"L1"})");
    expect_copper_error(invalid, "copper");
    invalid = copper_base;
    invalid["layers"][0]["regions"] = jsoncons::json::array();
    expect_copper_error(invalid, "regions");
    invalid = copper_base;
    invalid["layers"][0]["copper"].erase("fraction");
    invalid["layers"][0]["copper"]["polygons"] =
        jsoncons::json::parse(R"({"file":"poly.bin","layer":"L1"})");
    invalid["external_refs"] = jsoncons::json::parse(
        R"([{"path":"poly.bin","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}])");
    {
        std::ofstream out(copper_path);
        out << invalid;
    }
    {
        std::ofstream out(copper_path.parent_path() / "poly.bin", std::ios::binary);
        out << "self-made polyset placeholder";
    }
    auto sha_mismatch = load_stack(copper_path, schema02);
    expect(!sha_mismatch && sha_mismatch.error().path.find("sha256") != std::string::npos);
    std::filesystem::remove(copper_path.parent_path() / "poly.bin");
    std::filesystem::remove(copper_path);
    expect(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::ifstream in(root / "schema/examples/fcbga_demo.stack.json");
    auto base = jsoncons::json::parse(in);
    const auto path = std::filesystem::current_path() / "negative.stack.json";
    for (int i = 0; i < 15; ++i) {
        auto j = base;
        if (i < 2) {
            j["external_refs"] = jsoncons::json::parse(
                R"([{"path":"https://bad","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}])");
            if (i == 1) {
                j["external_refs"][0]["path"] = "external.bin";
                j["external_refs"][0].erase("sha256");
            }
        }
        if (i == 2)
            j["activation_groups"][0]["members"][0] = "group:assembly";
        if (i == 3)
            j["layers"][1]["name"] = "substrate";
        if (i == 4)
            j["activation_groups"][0]["members"][0] = "layer:absent";
        if (i == 5)
            j["layers"][0]["material"] = "SAC305";
        if (i == 6)
            j["layers"][2]["regions"][0]["footprint"]["box"][2] = 1;
        if (i == 7 || i == 8)
            j["external_refs"] = jsoncons::json::parse(
                i == 7
                    ? R"([{"path":"../escape","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}])"
                    : R"([{"path":"missing","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}])");
        if (i == 9)
            j["symmetry"] = "quarter";
        if (i == 10)
            j["symmetry"] = "invalid";
        if (i == 11)
            j["layers"][0]["name"] = "void";
        if (i == 12)
            j["layers"][2]["regions"][0]["name"] = "void";
        if (i == 13)
            j["layers"][0]["body"] = "void";
        if (i == 14)
            j["activation_groups"][0]["name"] = "void";
        {
            std::ofstream out(path);
            out << j;
        }
        auto result = load_stack(path, schema);
        expect(!result);
        expect(!result.error().reason.empty());
        const std::string paths[] = {"external_refs",
                                     "external_refs",
                                     "activation_groups",
                                     "layers",
                                     "activation_groups",
                                     "material",
                                     "footprint",
                                     "external_refs",
                                     "external_refs",
                                     "symmetry",
                                     "symmetry",
                                     "name",
                                     "regions",
                                     "body",
                                     "activation_groups"};
        expect(result.error().path.find(paths[i]) != std::string::npos);
    }
    auto positive = base;
    positive["layers"][0]["material"] = "SAC305_ball_eq";
    {
        std::ofstream out(path);
        out << positive;
    }
    expect(bool(load_stack(path, schema)));
    auto external = std::filesystem::current_path() / "external.bin";
    {
        std::ofstream bytes(external);
        bytes << "abc";
    }
    base["external_refs"] = jsoncons::json::parse(
        R"([{"path":"external.bin","sha256":"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}])");
    {
        std::ofstream out(path);
        out << base;
    }
    expect(bool(load_stack(path, schema)));
    {
        std::ofstream bytes(external);
        bytes << "abd";
    }
    auto mismatch = load_stack(path, schema);
    expect(!mismatch && mismatch.error().reason == "digest mismatch");
    std::filesystem::remove(external);
    std::filesystem::remove(path);

    Polyset polyset;
    polyset.dbu_m = 1e-9;
    PolysetLayer polygons;
    polygons.name = "L1";
    polygons.ring_offsets.reserve(10001);
    polygons.coordinates.reserve(80000);
    polygons.ring_offsets.push_back(0);
    for (std::int64_t i = 0; i < 10000; ++i) {
        polygons.coordinates.insert(polygons.coordinates.end(), {i, 0, i + 1, 0, i + 1, 1, i, 1});
        polygons.ring_offsets.push_back((i + 1) * 4);
    }
    polyset.layers.push_back(std::move(polygons));
    auto poly_path = std::filesystem::current_path() / "polyset-roundtrip.bin";
    expect(bool(write_polyset(poly_path, polyset)));
    auto roundtrip = read_polyset(poly_path);
    expect(bool(roundtrip));
    expect(roundtrip.value().dbu_m == polyset.dbu_m);
    expect(roundtrip.value().layers.size() == polyset.layers.size());
    expect(roundtrip.value().layers[0].name == polyset.layers[0].name);
    expect(roundtrip.value().layers[0].ring_offsets == polyset.layers[0].ring_offsets);
    expect(roundtrip.value().layers[0].coordinates == polyset.layers[0].coordinates);
    std::filesystem::remove(poly_path);
}
