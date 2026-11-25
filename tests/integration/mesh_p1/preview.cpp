#ifdef CHIPCAE_ENABLE_KOKKOS
#include <Kokkos_Core.hpp>
#endif
#include "../../../preview/src/mesh_digest.hpp"
#include "../../../schema/src/sha256.hpp"
#include "support.hpp"

#include <chrono>
#include <cstdlib>
#include <hdf5.h>
#include <iostream>
#include <jsoncons/json.hpp>
#include <sstream>
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
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::ScopeGuard guard;
#endif
    expect(argc == 2 || argc == 4, "preview executable argument");
    auto dir = fs::temp_directory_path() /
               ("chipcae-preview-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    try {
        auto fixture = fs::path(K1_ROOT) / "schema/examples/fcbga_demo.stack.json";
        auto output = dir / "grid";
        auto invoke = [&](const std::string& args,
                          int expected_code,
                          double expected_max_ratio,
                          int expected_levels = -1,
                          std::string expected_n0 = "",
                          bool use_mpi = true,
                          std::string expected_error_path = "") {
            jsoncons::json summary;
            auto prefix = argc == 4 && use_mpi ? shell(argv[2]) + " " + argv[3] + " " : "";
            auto command = prefix + shell(argv[1]) + " " + args + " >" +
                           shell((dir / "stdout").string()) + " 2>" +
                           shell((dir / "stderr").string());
            int code = std::system(command.c_str());
            if (!WIFEXITED(code) || WEXITSTATUS(code) != expected_code) {
                std::ifstream diagnostic(dir / "stderr");
                std::cerr << "Expected exit " << expected_code << ", actual status " << code << ": "
                          << diagnostic.rdbuf();
            }
            expect(WIFEXITED(code) && WEXITSTATUS(code) == expected_code, "CLI exact exit status");
            std::ifstream stream(dir / "stdout");
            std::string line, last;
            int count = 0, terminal = 0;
            while (std::getline(stream, line)) {
                auto event = jsoncons::json::parse(line);
                expect(event.at("stage").is_string(), "stage string");
                expect(event.at("progress").is_number(), "numeric progress");
                expect(event.at("message").is_string(), "message string");
                last = event.at("stage").as<std::string>();
                if (last == "success" || last == "error")
                    ++terminal;
                if (last == "error" && !expected_error_path.empty())
                    expect(event.at("message").as<std::string>().starts_with(expected_error_path +
                                                                             ": "),
                           "terminal mesh error path");
                if (last == "success") {
                    auto params = jsoncons::json::parse(event.at("message").as<std::string>());
                    expect(params.at("grid").is_string(), "effective grid");
                    if (params.at("max_ratio").as<double>() != expected_max_ratio)
                        std::cerr << "Expected max_ratio " << expected_max_ratio << ", actual "
                                  << params.at("max_ratio").as<double>() << '\n';
                    expect(params.at("max_ratio").as<double>() == expected_max_ratio,
                           "effective max ratio");
                    expect(
                        params.at("L").as<int>() ==
                                (expected_levels < 0 ? (argc == 4 ? 0 : 1) : expected_levels) &&
                            params.at("n0").as<std::string>() ==
                                (expected_n0.empty() ? (argc == 4 ? "9x9" : "5x5") : expected_n0),
                        "effective hierarchy");
                    for (const auto& field : params.object_range())
                        expect(event.at(field.key()) == field.value(),
                               "top-level and success.message summaries agree");
                    summary = params;
                }
                ++count;
            }
            if (expected_code == 0)
                expect(terminal == 1 && count == 4,
                       "exactly one terminal and rank-zero progress events");
            expect(count > 0 && last == (expected_code == 0 ? "success" : "error"),
                   "terminal JSON event");
            return summary;
        };
        auto ratio_free_base = shell(fixture.string()) + " --nx 2 --ny 2 --levels 0";
        auto base = ratio_free_base + " --max-ratio 10";
        invoke(base + " -o " + shell(output.string()), 0, 10);
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
        options.ranks = argc == 4 ? 2 : 1;
        auto mesh = chipcae::mesh::build_tensor_grid(stack.value(), options);
        expect(bool(mesh), "fixture mesh");
        for (int axis : {0, 1}) {
            auto invalid_mesh = mesh.value();
            invalid_mesh.grid_report.padding[axis] = invalid_mesh.axes[axis].size();
            bool rejected = false;
            try {
                chipcae::preview::detail::digest(invalid_mesh);
            } catch (const std::invalid_argument& error) {
                rejected = std::string(error.what()) == "mesh padding exceeds axis cell count";
            }
            expect(rejected, "digest rejects padding beyond either axis cell count");
        }
        expect(static_cast<std::size_t>(nc) == mesh.value().attribute.size(), "cell coverage");
        std::map<std::string, std::int32_t> layers;
        for (const auto& entry : mesh.value().attr_table.entries)
            if (!layers.contains(entry.layer))
                layers[entry.layer] = static_cast<std::int32_t>(layers.size() + 1);
        for (auto name : {"layer_names", "material_names"}) {
            std::vector<std::string> expected;
            if (std::string(name) == "layer_names") {
                for (const auto& layer : stack.value().layers)
                    expected.push_back(layer.name);
            } else {
                expected.resize(mesh.value().attr_table.materials.size());
                for (const auto& [material, id] : mesh.value().attr_table.materials)
                    expected.at(id - 1) = material;
            }
            auto path = std::string("VTKHDF/FieldData/") + name;
            dataset = H5Dopen2(file, path.c_str(), H5P_DEFAULT);
            expect(dataset >= 0, "name array exists");
            hid_t type = H5Dget_type(dataset), space = H5Dget_space(dataset);
            hsize_t count = 0;
            expect(H5Tget_class(type) == H5T_STRING && H5Tget_cset(type) == H5T_CSET_UTF8,
                   "UTF-8 string array");
            expect(H5Sget_simple_extent_ndims(space) == 1 &&
                       H5Sget_simple_extent_dims(space, &count, nullptr) >= 0 &&
                       count == expected.size(),
                   "name array shape");
            auto width = H5Tget_size(type);
            std::vector<char> bytes(count * width);
            expect(H5Dread(dataset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, bytes.data()) >= 0,
                   "name array read");
            for (std::size_t i = 0; i < expected.size(); ++i)
                expect(std::string(bytes.data() + i * width) == expected[i], "ID to name mapping");
            H5Sclose(space);
            H5Tclose(type);
            H5Dclose(dataset);
        }
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
        invoke(
            ratio_free_base + " --max-ratio 12.5 -o " + shell((dir / "ratio").string()), 0, 12.5);
        auto exact = dir / "exact.vtkhdf";
        auto surface = dir / "custom.eb.vtkhdf";
        invoke(
            base + " -o " + shell(exact.string()) + " --surface " + shell(surface.string()), 0, 10);
        expect(fs::exists(exact) && fs::exists(surface) && !fs::exists(exact.string() + ".vtkhdf"),
               "exact output paths");
        invoke(base + " -o " + shell((dir / "rename-failure").string()) + " --surface " +
                   shell((dir / "missing/surface.vtkhdf").string()),
               1,
               10);
        auto dot = dir / ".vtkhdf";
        invoke(base + " -o " + shell(dot.string()), 0, 10);
        expect(fs::exists(dot) && !fs::exists(dot.string() + ".vtkhdf"), "leading dot output");
        auto real = dir / "real";
        fs::create_directory(real);
        fs::create_directory_symlink(real, dir / "link");
        auto alias_grid = real / "alias.vtkhdf";
        invoke(base + " -o " + shell(alias_grid.string()) + " --surface " +
                   shell((dir / "link/alias.vtkhdf").string()),
               1,
               10);
        expect(!fs::exists(alias_grid), "symlink alias rejected before writing");
        invoke(shell(fixture.string()) + " --levels 32 --max-ratio 10 -o " +
                   shell((dir / "levels").string()),
               4,
               10);
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
        invoke(shell(two.string()) + " -o " + shell((dir / "two").string()), 1, 10);
        std::ifstream diagnostic(dir / "stderr");
        std::string message((std::istreambuf_iterator<char>(diagnostic)), {});
        expect(message.find("duplicate polyset layer name: L1") != std::string::npos,
               "reject ambiguous sidecar names");
        expect(!fs::exists(dir / "two.vtkhdf"), "no ambiguous copper output");
#endif
        auto stderr_contains = [&](const char* path) {
            std::ifstream diagnostic(dir / "stderr");
            std::string message((std::istreambuf_iterator<char>(diagnostic)), {});
            expect(message.find(path) != std::string::npos, "mesh error path in stderr");
        };
        auto bad = dir / "bad";
        for (const auto& args : std::vector<std::string>{
                 base,
                 shell((dir / "absent.stack.json").string()) + " -o " + shell(bad.string()),
                 shell(fixture.string()) + " --nx 2 -o " + shell(bad.string()),
                 base + " --nx 3 -o " + shell(bad.string()),
                 base + " --levels nope -o " + shell(bad.string())}) {
            invoke(args, 1, 10);
            expect(!fs::exists(bad.string() + ".vtkhdf") &&
                       !fs::exists(bad.string() + ".eb.vtkhdf"),
                   "no failed output");
        }
        for (const auto& [ratio_args, diagnostic] :
             std::vector<std::pair<std::string, std::string>>{
                 {"--max-ratio nan", "invalid max ratio: nan"},
                 {"--max-ratio 0.5", "invalid max ratio: 0.5"},
                 {"--max-ratio 10junk", "invalid max ratio: 10junk"},
                 {"--max-ratio", "missing value for --max-ratio"},
                 {"--max-ratio 10 --max-ratio 12.5", "duplicate option: --max-ratio"}}) {
            invoke(ratio_free_base + " -o " + shell(bad.string()) + " " + ratio_args, 1, 10);
            stderr_contains(diagnostic.c_str());
            expect(!fs::exists(bad.string() + ".vtkhdf") &&
                       !fs::exists(bad.string() + ".eb.vtkhdf"),
                   "no invalid ratio output");
        }
        invoke(shell((dir / std::string("invalid-\xff.stack.json")).string()) + " -o " +
                   shell(bad.string()),
               1,
               10);
        auto invalid = dir / "invalid.stack.json";
        std::ofstream(invalid) << "{\"bad\": true}";
        invoke(shell(invalid.string()) + " -o " + shell(bad.string()), 1, 10);
        expect(!fs::exists(bad.string() + ".vtkhdf"), "no invalid stack output");
        auto tiny = dir / "tiny.stack.json";
        const std::string tiny_input = R"({"id":"tiny","unit":"m","outline":{"box":[0,0,7,5]},
"symmetry":"none","package_type":"FCBGA","layers":[{"name":"solid","thickness":1,
"kind":"Homogenized","material":"solid_eq","n_elem_min":1,"grading":1}]}
)";
        std::ofstream(tiny) << tiny_input;
        // Captured on unmodified origin/main aafc877142d5ebe2d16befe80f8c830384beeb49,
        // using the exact input above and --nx 7 --ny 5 --levels 2, serial and two ranks.
        // Both: nx0=4, ny0=3, L=1, nz=1, nodes=126, cells=48, padded_cells=2.
        // The same input with --nx 50001 --ny 1 --levels 1 exited 1 (cell_budget).
        // These retrospective pre-change captures were verified on 2026-11-18.
        expect(chipcae::mesh::sha256(tiny_input) ==
                   "94c3f4d3cbae4df796d749c1649d7bbe6941c7736aca3a17813d1ac6d7f71063",
               "baseline input SHA");
        int acceptance_failures = 0;
        auto acceptance = [&](const char* name, auto check) {
            try {
                check();
            } catch (const std::exception& error) {
                ++acceptance_failures;
                std::cerr << name << ": " << error.what() << '\n';
            }
        };
        for (bool use_mpi : (argc == 4 ? std::vector<bool>{false, true} : std::vector<bool>{false}))
            acceptance(use_mpi ? "K1 omitted ratio MPI" : "K1 omitted ratio serial", [&] {
                auto stem = dir / (use_mpi ? "k1-default-mpi" : "k1-default-serial");
                invoke(ratio_free_base + " -o " + shell(stem.string()),
                       14,
                       2.0,
                       -1,
                       "",
                       use_mpi,
                       "layers");
                stderr_contains("interface adjacent size ratio exceeds max_ratio");
                expect(!fs::exists(stem.string() + ".vtkhdf") &&
                           !fs::exists(stem.string() + ".eb.vtkhdf"),
                       "no K1 omitted ratio output");
            });
        for (bool use_mpi : (argc == 4 ? std::vector<bool>{false, true} : std::vector<bool>{false}))
            acceptance(use_mpi ? "default max ratio MPI" : "default max ratio serial", [&] {
                auto default_stack = chipcae::mesh::load_stack(
                    tiny, fs::path(K1_ROOT) / "schema/stack-0.2.schema.json");
                expect(bool(default_stack), "default ratio stack");
                chipcae::mesh::GridOptions default_options{};
                default_options.nx = 7;
                default_options.ny = 5;
                default_options.levels = 1;
                default_options.ranks = argc == 4 && use_mpi ? 2 : 1;
                expect(default_options.max_ratio == 2.0, "reference default max ratio is 2");
                auto default_mesh =
                    chipcae::mesh::build_tensor_grid(default_stack.value(), default_options);
                expect(bool(default_mesh), "default ratio reference mesh");
                auto stem = dir / (use_mpi ? "default-mpi" : "default-serial");
                invoke(shell(tiny.string()) + " --nx 7 --ny 5 --levels 1 -o " +
                           shell(stem.string()),
                       0,
                       2.0,
                       0,
                       "7x5",
                       use_mpi);
                auto default_grid = fs::path(stem.string() + ".vtkhdf");
                expect(fs::exists(default_grid), "default ratio grid output exists");
                hid_t default_file = H5Fopen(default_grid.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
                expect(default_file >= 0, "open default ratio output");
                hid_t default_cells = H5Dopen2(default_file, "VTKHDF/NumberOfCells", H5P_DEFAULT);
                std::int64_t default_nc = 0;
                expect(default_cells >= 0 && H5Dread(default_cells,
                                                     H5T_NATIVE_INT64,
                                                     H5S_ALL,
                                                     H5S_ALL,
                                                     H5P_DEFAULT,
                                                     &default_nc) >= 0,
                       "read default ratio cell count");
                H5Dclose(default_cells);
                H5Fclose(default_file);
                expect(default_nc > 0, "positive default ratio cell count");
                expect(static_cast<std::size_t>(default_nc) ==
                           default_mesh.value().attribute.size(),
                       "default ratio cell coverage matches GridOptions reference");
            });
        for (bool use_mpi : (argc == 4 ? std::vector<bool>{false, true} : std::vector<bool>{false}))
            acceptance(use_mpi ? "tiny padding and SHA MPI" : "tiny padding and SHA serial", [&] {
                constexpr int nx_req = 7, ny_req = 5;
                auto summary =
                    invoke(shell(tiny.string()) + " --nx " + std::to_string(nx_req) + " --ny " +
                               std::to_string(ny_req) + " --levels 2 --max-ratio 10 -o " +
                               shell((dir / (use_mpi ? "padded-mpi" : "padded-serial")).string()),
                           0,
                           10,
                           1,
                           "4x3",
                           use_mpi);
                for (auto [key, value] : {std::pair{"nx0", 4}, {"ny0", 3}, {"L", 1}, {"nz", 1}})
                    expect(summary.at(key).is_int64() && summary.at(key).as<int>() == value,
                           "tiny hierarchy integer summary");
                // Verified against pre-change aafc877142d5ebe2d16befe80f8c830384beeb49.
                expect(summary.at("mesh_sha256").as<std::string>() ==
                           "31ccb0783121d620d23890e5bab6e87fa6f86cc4cc8bc521ff9138c715ac49a6",
                       "padded mesh SHA matches pre-change baseline");
                const auto& padded = summary.at("padded_cells");
                expect(padded.is_int64() && padded.as<int>() == 13 && padded.as<int>() > 0,
                       "padded_cells is integer 13 and positive");
                const auto scale = 1LL << summary.at("L").as<int>();
                const auto expected_padding =
                    (summary.at("nx0").as<int>() * scale * summary.at("ny0").as<int>() * scale -
                     nx_req * ny_req) *
                    summary.at("nz").as<int>();
                expect(padded.as<int64_t>() == expected_padding,
                       "padded_cells matches reported hierarchy dimensions and request");
            });
        auto multi = jsoncons::json::parse(tiny_input);
        multi["layers"][0]["n_elem_min"] = 3;
        auto second_layer = multi["layers"][0];
        second_layer["name"] = "second";
        multi["layers"].push_back(second_layer);
        auto multi_path = dir / "multi.stack.json";
        std::ofstream(multi_path) << multi;
        for (bool use_mpi :
             (argc == 4 ? std::vector<bool>{false, true} : std::vector<bool>{false})) {
            auto summary =
                invoke(shell(multi_path.string()) + " --nx 7 --ny 4 --levels 2 --max-ratio 10 -o " +
                           shell((dir / (use_mpi ? "multi-mpi" : "multi-serial")).string()),
                       0,
                       10,
                       1,
                       "4x2",
                       use_mpi);
            expect(summary.at("nz").as<int>() == 6, "two layers with three z cells each");
            // Finest grid is 8x4; padding is (1,0), so four cells per z slice.
            expect(summary.at("padded_cells").is_int64() &&
                       summary.at("padded_cells").as<int>() == 24,
                   "asymmetric padding includes every z slice");
        }
        invoke(base + " --polyset " + shell((dir / "missing.poly").string()) + " -o " +
                   shell((dir / "missing-polyset").string()),
               1,
               10,
               -1,
               "",
               false);
        fs::create_directory(dir / "blocked.vtkhdf");
        invoke(base + " -o " + shell((dir / "blocked").string()), 1, 10, -1, "", false);
        acceptance("tiny cell budget", [&] {
            invoke(shell(tiny.string()) + " --nx 50001 --ny 1 --levels 1 --max-ratio 10 -o " +
                       shell((dir / "budget").string()),
                   14,
                   10,
                   -1,
                   "",
                   false);
            stderr_contains("cell_budget");
        });
        if (argc == 4)
            acceptance("tiny hierarchy alias", [&] {
                invoke(shell(tiny.string()) + " --nx 2 --ny 2 --levels 2 --max-ratio 10 -o " +
                           shell((dir / "hierarchy").string()),
                       4,
                       10,
                       -1,
                       "",
                       true,
                       "hierarchy/levels");
                stderr_contains("hierarchy/levels");
            });
        expect(acceptance_failures == 0, "preview acceptance cases");
        fs::remove_all(dir);
    } catch (...) {
        std::cerr << "Preview artifacts: " << dir << '\n';
        throw;
    }
}
