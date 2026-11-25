// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Zhiwei Duan

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <hdf5.h>
#include <iostream>
#include <jsoncons/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

static void expect(bool ok, const std::string& why) {
    if (!ok)
        throw std::runtime_error(why);
}

static std::string read_text(const fs::path& path) {
    std::ifstream in(path);
    expect(in.good(), "open " + path.string());
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

static std::string shell(const std::string& value) {
    std::string out = "'";
    for (char c : value)
        out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}

struct Layer {
    std::string name;
    std::string material;
    double thickness = 0;
    int n_elem_min = 0;
};

static std::vector<Layer> parse_layers(const jsoncons::json& stack) {
    expect(stack.contains("layers") && stack["layers"].is_array(), "layers array");
    std::vector<Layer> layers;
    for (const auto& obj : stack["layers"].array_range()) {
        Layer layer;
        layer.name = obj["name"].as<std::string>();
        layer.material = obj["material"].as<std::string>();
        layer.thickness = obj["thickness"].as<double>();
        layer.n_elem_min = obj["n_elem_min"].as<int>();
        layers.push_back(layer);
    }
    expect(!layers.empty(), "parsed layers");
    return layers;
}

template <class T>
static std::vector<T> h5_read(hid_t file, const char* name, hid_t type) {
    hid_t d = H5Dopen2(file, name, H5P_DEFAULT);
    expect(d >= 0, std::string("missing dataset ") + name);
    hid_t s = H5Dget_space(d);
    expect(s >= 0, "dataspace");
    const int rank = H5Sget_simple_extent_ndims(s);
    std::vector<hsize_t> dims(static_cast<std::size_t>(rank));
    H5Sget_simple_extent_dims(s, dims.data(), nullptr);
    hsize_t n = 1;
    for (auto dim : dims)
        n *= dim;
    std::vector<T> values(static_cast<std::size_t>(n));
    expect(H5Dread(d, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.data()) >= 0, "h5 read");
    H5Sclose(s);
    H5Dclose(d);
    return values;
}

static std::vector<std::string> h5_strings(hid_t file, const char* name) {
    hid_t d = H5Dopen2(file, name, H5P_DEFAULT);
    expect(d >= 0, std::string("missing string dataset ") + name);
    hid_t type = H5Dget_type(d), space = H5Dget_space(d);
    expect(H5Tget_class(type) == H5T_STRING, "string class");
    hsize_t count = 0;
    expect(H5Sget_simple_extent_ndims(space) == 1 &&
               H5Sget_simple_extent_dims(space, &count, nullptr) >= 0,
           "string shape");
    auto width = H5Tget_size(type);
    std::vector<char> bytes(static_cast<std::size_t>(count * width));
    expect(H5Dread(d, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, bytes.data()) >= 0, "string read");
    std::vector<std::string> out(static_cast<std::size_t>(count));
    for (hsize_t i = 0; i < count; ++i)
        out[static_cast<std::size_t>(i)] = std::string(bytes.data() + i * width);
    H5Sclose(space);
    H5Tclose(type);
    H5Dclose(d);
    return out;
}

static double
hex_volume(const std::vector<double>& pts, const std::vector<std::int64_t>& conn, std::size_t off) {
    // Axis-aligned hex: bbox of the 8 corners.
    double mn[3] = {1e300, 1e300, 1e300};
    double mx[3] = {-1e300, -1e300, -1e300};
    for (int n = 0; n < 8; ++n) {
        auto id = static_cast<std::size_t>(conn[off + static_cast<std::size_t>(n)]);
        for (int d = 0; d < 3; ++d) {
            double v = pts[3 * id + static_cast<std::size_t>(d)];
            mn[d] = std::min(mn[d], v);
            mx[d] = std::max(mx[d], v);
        }
    }
    return (mx[0] - mn[0]) * (mx[1] - mn[1]) * (mx[2] - mn[2]);
}

int main(int argc, char** argv) {
    fs::path dir;
    try {
        expect(argc == 2, "usage: preview_zalign <chipcae-preview>");
        const fs::path root(CHIPCAE_SOURCE_DIR);
        const auto fixture = root / "schema/examples/fcbga_demo.stack.json";
        // STK-2 declares k1.json as the shared interface/volume contract
        // (mesh/background/thresholds/k1.json). Read those keys at runtime;
        // do not bake a second copy of the numbers into this test.
        const auto thresholds = root / "mesh/background/thresholds/k1.json";
        const auto k1 = jsoncons::json::parse(read_text(thresholds));
        const double interface_relative = k1["interface_relative"].as<double>();
        const double volume_relative = k1["volume_relative"].as<double>();
        const auto stack = jsoncons::json::parse(read_text(fixture));
        auto layers = parse_layers(stack);
        std::string die_material;
        std::vector<double> die;
        std::size_t die_layer = layers.size();
        for (std::size_t i = 0; i < layers.size(); ++i) {
            if (layers[i].name != "die_layer")
                continue;
            for (const auto& region : stack["layers"][i]["regions"].array_range()) {
                if (region["name"].as<std::string>() == "die") {
                    die_layer = i;
                    die_material = region["material"].as<std::string>();
                    die = region["footprint"]["box"].as<std::vector<double>>();
                }
            }
        }
        expect(die_layer < layers.size() && die.size() == 4, "die region and footprint box");
        expect(layers.size() == 3, "fcbga_demo has 3 layers");

        dir = fs::temp_directory_path() / ("preview-zalign-" + std::to_string(getpid()));
        fs::create_directories(dir);
        const auto out_prefix = dir / "grid";
        const auto grid = fs::path(out_prefix.string() + ".vtkhdf");
        std::ostringstream cmd;
        cmd << shell(argv[1]) << " " << shell(fixture.string())
            << " --nx 4 --ny 4 --levels 1 --max-ratio 10 -o " << shell(out_prefix.string()) << " >"
            << shell((dir / "stdout").string()) << " 2>" << shell((dir / "stderr").string());
        int code = std::system(cmd.str().c_str());
        expect(WIFEXITED(code) && WEXITSTATUS(code) == 0, "chipcae-preview exit 0");
        expect(fs::exists(grid), "grid.vtkhdf exists");

        hid_t file = H5Fopen(grid.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
        expect(file >= 0, "open vtkhdf");
        auto ncells_v = h5_read<std::int64_t>(file, "VTKHDF/NumberOfCells", H5T_NATIVE_INT64);
        expect(ncells_v.size() == 1, "NumberOfCells scalar");
        const auto nc = static_cast<std::size_t>(ncells_v[0]);
        // Public STK-2 v1 cardinalities: tests/preview/README.md, for this
        // fixture with --nx 4 --ny 4 --levels 1 (cells, not point planes).
        expect(nc == 17 * 17 * 6, "17x17x6 cells");
        auto points = h5_read<double>(file, "VTKHDF/Points", H5T_NATIVE_DOUBLE);
        expect(points.size() % 3 == 0, "points xyz");
        auto offsets = h5_read<std::int64_t>(file, "VTKHDF/Offsets", H5T_NATIVE_INT64);
        expect(offsets.size() == nc + 1,
               "Offsets has NumberOfCells+1 entries (writer starts at 0)");
        auto connectivity = h5_read<std::int64_t>(file, "VTKHDF/Connectivity", H5T_NATIVE_INT64);
        auto layer_id = h5_read<std::int32_t>(file, "VTKHDF/CellData/layer_id", H5T_NATIVE_INT32);
        auto material_id =
            h5_read<std::int32_t>(file, "VTKHDF/CellData/material_id", H5T_NATIVE_INT32);
        expect(layer_id.size() == nc && material_id.size() == nc, "cell field length");
        // Public preview VTKHDF contract, preview/README.md "Cell fields are
        // layer_id and material_id": CellData layer_id/material_id (int32,
        // 1-based) and FieldData layer_names/material_names (index i -> ID i+1).
        auto layer_names = h5_strings(file, "VTKHDF/FieldData/layer_names");
        auto material_names = h5_strings(file, "VTKHDF/FieldData/material_names");
        H5Fclose(file);

        expect(layer_names.size() == layers.size(), "layer_names count");
        for (std::size_t i = 0; i < layers.size(); ++i)
            expect(layer_names[i] == layers[i].name, "layer_names order bottom-to-top");

        auto material_index = [&](const std::string& name) -> std::int32_t {
            for (std::size_t i = 0; i < material_names.size(); ++i)
                if (material_names[i] == name)
                    return static_cast<std::int32_t>(i + 1);
            throw std::runtime_error("missing material " + name);
        };
        const auto id_die = material_index(die_material);
        std::vector<std::int32_t> default_materials;
        for (const auto& layer : layers)
            default_materials.push_back(material_index(layer.material));

        std::vector<double> z_planes, x_planes, y_planes;
        double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
        for (std::size_t i = 0; i < points.size() / 3; ++i) {
            xmin = std::min(xmin, points[3 * i]);
            xmax = std::max(xmax, points[3 * i]);
            ymin = std::min(ymin, points[3 * i + 1]);
            ymax = std::max(ymax, points[3 * i + 1]);
            x_planes.push_back(points[3 * i]);
            y_planes.push_back(points[3 * i + 1]);
            z_planes.push_back(points[3 * i + 2]);
        }
        for (auto* planes : {&x_planes, &y_planes}) {
            std::sort(planes->begin(), planes->end());
            planes->erase(std::unique(planes->begin(), planes->end()), planes->end());
            expect(planes->size() == 18, "18 planes for 17 in-plane cells");
        }
        std::sort(z_planes.begin(), z_planes.end());
        z_planes.erase(std::unique(z_planes.begin(),
                                   z_planes.end(),
                                   [](double a, double b) { return std::abs(a - b) <= 1e-15; }),
                       z_planes.end());
        expect(z_planes.size() == 7, "7 z planes for 6 element layers");

        double H = 0;
        for (const auto& layer : layers)
            H += layer.thickness;
        const double iface_tol = interface_relative * H;
        expect(std::abs(z_planes.front()) <= iface_tol, "bottom z = 0");
        std::vector<double> interfaces{0};
        double zacc = 0;
        for (const auto& layer : layers) {
            zacc += layer.thickness;
            interfaces.push_back(zacc);
        }
        for (double z : interfaces) {
            auto it = std::find_if(z_planes.begin(), z_planes.end(), [&](double p) {
                return std::abs(p - z) <= iface_tol;
            });
            expect(it != z_planes.end(), "interface z falls on a mesh plane");
        }

        std::vector<std::vector<double>> zc_by_layer(layers.size());
        double volume = 0;
        for (std::size_t c = 0; c < nc; ++c) {
            expect(offsets[c + 1] - offsets[c] == 8, "8-node hex");
            const auto off = static_cast<std::size_t>(offsets[c]);
            double cx = 0, cy = 0, cz = 0;
            for (int n = 0; n < 8; ++n) {
                auto id = static_cast<std::size_t>(connectivity[off + static_cast<std::size_t>(n)]);
                cx += points[3 * id];
                cy += points[3 * id + 1];
                cz += points[3 * id + 2];
            }
            cx /= 8;
            cy /= 8;
            cz /= 8;
            volume += hex_volume(points, connectivity, off);
            expect(layer_id[c] >= 1 && static_cast<std::size_t>(layer_id[c]) <= layers.size(),
                   "layer_id 1-based");
            const auto li = static_cast<std::size_t>(layer_id[c] - 1);
            zc_by_layer[li].push_back(cz);
            expect(cz > interfaces[li] && cz < interfaces[li + 1],
                   "layer_id matches geometric layer");
            const bool in_die =
                li == die_layer && cx >= die[0] && cy >= die[1] && cx <= die[2] && cy <= die[3];
            expect(material_id[c] == (in_die ? id_die : default_materials[li]),
                   "fixture layer and region material");
        }
        for (std::size_t i = 0; i < layers.size(); ++i) {
            auto zs = zc_by_layer[i];
            std::sort(zs.begin(), zs.end());
            zs.erase(std::unique(zs.begin(),
                                 zs.end(),
                                 [](double a, double b) { return std::abs(a - b) <= 1e-12; }),
                     zs.end());
            expect(static_cast<int>(zs.size()) >= layers[i].n_elem_min,
                   "z element layers >= n_elem_min");
        }
        const auto outline = stack["outline"]["box"].as<std::vector<double>>();
        expect(outline.size() == 4, "outline box");
        expect(std::abs(xmin - outline[0]) <= iface_tol &&
                   std::abs(ymin - outline[1]) <= iface_tol &&
                   std::abs(xmax - outline[2]) <= iface_tol &&
                   std::abs(ymax - outline[3]) <= iface_tol,
               "mesh matches fixture outline");
        const double area = (outline[2] - outline[0]) * (outline[3] - outline[1]);
        const double expected = area * H;
        expect(std::abs(volume - expected) <= volume_relative * expected, "volume conservation");

        std::cout << "preview_zalign: cells=" << nc << " planes_z=" << z_planes.size()
                  << " volume=" << volume << " expected=" << expected << " ok\n";
        fs::remove_all(dir);
        return 0;
    } catch (const std::exception& ex) {
        std::error_code cleanup_error;
        if (!dir.empty())
            fs::remove_all(dir, cleanup_error);
        std::cerr << "preview_zalign failed: " << ex.what() << "\n";
        return 1;
    }
}
