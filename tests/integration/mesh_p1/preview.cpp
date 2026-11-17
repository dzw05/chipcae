#include "../../../schema/src/sha256.hpp"
#include "support.hpp"

#include <chrono>
#include <cstdlib>
#include <hdf5.h>
#include <iostream>
#include <jsoncons/json.hpp>
#include <sys/wait.h>
using k1::expect;
namespace fs = std::filesystem;
std::string shell(const std::string& value) {
    std::string out = "'";
    for (char c : value)
        out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}
int main(int argc, char** argv) {
    expect(argc == 2 || argc == 4, "preview executable argument");
    auto dir = fs::temp_directory_path() /
               ("chipcae-preview-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    try {
        auto fixture = fs::path(K1_ROOT) / "schema/examples/fcbga_demo.stack.json";
        auto output = dir / "grid";
        auto invoke = [&](const std::string& args, int expected_code) {
            auto prefix = argc == 4 ? shell(argv[2]) + " " + argv[3] + " " : "";
            auto command = prefix + shell(argv[1]) + " " + args + " >" +
                           shell((dir / "stdout").string()) + " 2>" +
                           shell((dir / "stderr").string());
            int code = std::system(command.c_str());
            expect(WIFEXITED(code) && WEXITSTATUS(code) == expected_code, "CLI exact exit status");
            std::ifstream stream(dir / "stdout");
            std::string line, last;
            int count = 0;
            while (std::getline(stream, line)) {
                auto event = jsoncons::json::parse(line);
                expect(event.at("stage").is_string(), "stage string");
                expect(event.at("progress").is_number(), "numeric progress");
                expect(event.at("message").is_string(), "message string");
                last = event.at("stage").as<std::string>();
                ++count;
            }
            expect(count > 0 && last == (expected_code == 0 ? "success" : "error"),
                   "terminal JSON event");
        };
        auto base = shell(fixture.string()) + " --nx 2 --ny 2 --levels 0";
        invoke(base + " -o " + shell(output.string()), 0);
        auto grid = fs::path(output.string() + ".vtkhdf");
        expect(fs::exists(grid), "grid output exists");
        hid_t file = H5Fopen(grid.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
        expect(file >= 0, "open output");
        hid_t dataset = H5Dopen2(file, "VTKHDF/NumberOfCells", H5P_DEFAULT);
        std::int64_t nc = 0;
        expect(dataset >= 0 &&
                   H5Dread(dataset, H5T_NATIVE_INT64, H5S_ALL, H5S_ALL, H5P_DEFAULT, &nc) >= 0 &&
                   nc > 0,
               "positive cell count");
        H5Dclose(dataset);
        auto stack =
            chipcae::mesh::load_stack(fixture, fs::path(K1_ROOT) / "schema/stack-0.2.schema.json");
        expect(bool(stack), "fixture stack");
        chipcae::mesh::GridOptions options;
        // The K1 FCBGA fixture has a 10:1 interface cell-height ratio.
        options.max_ratio = 10;
        options.nx = options.ny = 2;
        auto mesh = chipcae::mesh::build_tensor_grid(stack.value(), options);
        expect(bool(mesh), "fixture mesh");
        expect(static_cast<std::size_t>(nc) == mesh.value().attribute.size(), "cell coverage");
        std::map<std::string, std::int32_t> layers;
        for (const auto& entry : mesh.value().attr_table.entries)
            if (!layers.contains(entry.layer))
                layers[entry.layer] = static_cast<std::int32_t>(layers.size() + 1);
        for (auto name : {"layer_id", "material_id", "cell_kind"}) {
            auto path = std::string("VTKHDF/CellData/") + name;
            dataset = H5Dopen2(file, path.c_str(), H5P_DEFAULT);
            expect(dataset >= 0, "field exists");
            hid_t type = H5Dget_type(dataset), space = H5Dget_space(dataset);
            expect(H5Tequal(type, H5T_NATIVE_INT32) > 0, "int32 field");
            hsize_t size = 0;
            expect(H5Sget_simple_extent_ndims(space) == 1 &&
                       H5Sget_simple_extent_dims(space, &size, nullptr) >= 0 &&
                       size == static_cast<hsize_t>(nc),
                   "field shape");
            std::vector<std::int32_t> values(static_cast<std::size_t>(nc));
            expect(
                H5Dread(dataset, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.data()) >=
                    0,
                "field read");
            for (std::size_t i = 0; i < values.size(); ++i) {
                const auto& entry = mesh.value().attr_table.entries[mesh.value().attribute[i] - 1];
                auto expected = std::string(name) == "layer_id"      ? layers[entry.layer]
                                : std::string(name) == "material_id" ? entry.material
                                                                     : 0;
                expect(values[i] == expected, "field values");
            }
            H5Sclose(space);
            H5Tclose(type);
            H5Dclose(dataset);
        }
        for (auto name : {"layer", "material"}) {
            hid_t old = -1;
            H5E_BEGIN_TRY {
                old = H5Dopen2(file, (std::string("VTKHDF/CellData/") + name).c_str(), H5P_DEFAULT);
            }
            H5E_END_TRY;
            expect(old < 0, "old dataset cannot open");
        }
        H5Fclose(file);
        std::cout << "K1 NumberOfCells=" << nc
                  << "; layer_id/material_id int32 values and shape verified\n";
        auto exact = dir / "exact.vtkhdf";
        auto surface = dir / "custom.eb.vtkhdf";
        invoke(base + " -o " + shell(exact.string()) + " --surface " + shell(surface.string()), 0);
        expect(fs::exists(exact) && fs::exists(surface) && !fs::exists(exact.string() + ".vtkhdf"),
               "exact output paths");
        invoke(base + " -o " + shell((dir / "rename-failure").string()) + " --surface " +
                   shell((dir / "missing/surface.vtkhdf").string()),
               1);
        auto dot = dir / ".vtkhdf";
        invoke(base + " -o " + shell(dot.string()), 0);
        expect(fs::exists(dot) && !fs::exists(dot.string() + ".vtkhdf"), "leading dot output");
        auto real = dir / "real";
        fs::create_directory(real);
        fs::create_directory_symlink(real, dir / "link");
        auto alias_grid = real / "alias.vtkhdf";
        invoke(base + " -o " + shell(alias_grid.string()) + " --surface " +
                   shell((dir / "link/alias.vtkhdf").string()),
               1);
        expect(!fs::exists(alias_grid), "symlink alias rejected before writing");
        invoke(shell(fixture.string()) + " --levels 32 -o " + shell((dir / "levels").string()), 4);
#ifdef CHIPCAE_TEST_ECAD
        std::ifstream copper_in(fs::path(K1_ROOT) / "schema/examples/fcbga_cu.stack.json");
        auto copper = jsoncons::json::parse(copper_in);
        copper["layers"].resize(2);
        copper.erase("activation_groups");
        copper["external_refs"] = jsoncons::json::array();
        for (int i = 0; i < 2; ++i) {
            auto name = i == 0 ? "a.poly" : "b.poly";
            chipcae::mesh::Polyset part;
            part.dbu_m = 1e-6;
            part.layers.push_back({"L1", {0, 4}, {0, 0, 1000 + i, 0, 1000 + i, 1000, 0, 1000}});
            expect(bool(chipcae::mesh::write_polyset(dir / name, part)), "write sidecar");
            std::ifstream bytes(dir / name, std::ios::binary);
            std::string data((std::istreambuf_iterator<char>(bytes)), {});
            jsoncons::json ref = jsoncons::json::object();
            ref["path"] = name;
            ref["sha256"] = chipcae::mesh::sha256(data);
            copper["external_refs"].push_back(ref);
            copper["layers"][i]["kind"] = "Patterned";
            copper["layers"][i].erase("regions");
            copper["layers"][i]["copper"] = jsoncons::json::parse(
                R"({"metal":"copper","polygons":{"file":"a.poly","layer":"L1"}})");
            copper["layers"][i]["copper"]["polygons"]["file"] = name;
        }
        auto two = dir / "two.stack.json";
        std::ofstream(two) << copper;
        expect(bool(chipcae::mesh::load_stack(two,
                                              fs::path(K1_ROOT) / "schema/stack-0.2.schema.json")),
               "valid two-sidecar stack");
        invoke(shell(two.string()) + " -o " + shell((dir / "two").string()), 1);
        std::ifstream diagnostic(dir / "stderr");
        std::string message((std::istreambuf_iterator<char>(diagnostic)), {});
        expect(message.find("duplicate polyset layer name: L1") != std::string::npos,
               "reject ambiguous sidecar names");
        expect(!fs::exists(dir / "two.vtkhdf"), "no ambiguous copper output");
#endif
        auto bad = dir / "bad";
        for (const auto& args : std::vector<std::string>{
                 base,
                 shell((dir / "absent.stack.json").string()) + " -o " + shell(bad.string()),
                 shell(fixture.string()) + " --nx 2 -o " + shell(bad.string()),
                 base + " --nx 3 -o " + shell(bad.string()),
                 base + " --levels nope -o " + shell(bad.string())}) {
            invoke(args, 1);
            expect(!fs::exists(bad.string() + ".vtkhdf") &&
                       !fs::exists(bad.string() + ".eb.vtkhdf"),
                   "no failed output");
        }
        invoke(shell((dir / std::string("invalid-\xff.stack.json")).string()) + " -o " +
                   shell(bad.string()),
               1);
        auto invalid = dir / "invalid.stack.json";
        std::ofstream(invalid) << "{\"bad\": true}";
        invoke(shell(invalid.string()) + " -o " + shell(bad.string()), 1);
        expect(!fs::exists(bad.string() + ".vtkhdf"), "no invalid stack output");
        fs::remove_all(dir);
    } catch (...) {
        std::cerr << "Preview artifacts: " << dir << '\n';
        throw;
    }
}
