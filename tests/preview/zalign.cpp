// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Zhiwei Duan

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <hdf5.h>
#include <iostream>
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

static double json_number(const std::string& text, const std::string& key) {
    const auto token = "\"" + key + "\"";
    auto pos = text.find(token);
    expect(pos != std::string::npos, "missing key " + key);
    pos = text.find(':', pos);
    expect(pos != std::string::npos, "missing colon for " + key);
    return std::stod(text.substr(pos + 1));
}

struct Layer {
    std::string name;
    std::string material;
    double thickness = 0;
    int n_elem_min = 0;
};

static std::string json_string_field(const std::string& obj, const std::string& key) {
    const auto token = "\"" + key + "\"";
    auto pos = obj.find(token);
    expect(pos != std::string::npos, "missing string " + key);
    pos = obj.find(':', pos);
    pos = obj.find('"', pos);
    expect(pos != std::string::npos, "missing quote " + key);
    auto end = obj.find('"', pos + 1);
    return obj.substr(pos + 1, end - pos - 1);
}

static std::vector<Layer> parse_layers(const std::string& text) {
    auto start = text.find("\"layers\"");
    expect(start != std::string::npos, "layers array");
    start = text.find('[', start);
    expect(start != std::string::npos, "layers [");
    int depth = 0;
    std::vector<std::string> objs;
    std::string cur;
    for (std::size_t i = start + 1; i < text.size(); ++i) {
        char c = text[i];
        if (c == '{') {
            if (depth == 0)
                cur.clear();
            ++depth;
        }
        if (depth >= 1)
            cur.push_back(c);
        if (c == '}') {
            --depth;
            if (depth == 0)
                objs.push_back(cur);
        }
        if (c == ']' && depth == 0)
            break;
    }
    std::vector<Layer> layers;
    for (const auto& obj : objs) {
        // Only top-level layer objects contain "n_elem_min".
        if (obj.find("\"n_elem_min\"") == std::string::npos)
            continue;
        Layer layer;
        layer.name = json_string_field(obj, "name");
        layer.material = json_string_field(obj, "material");
        layer.thickness = json_number(obj, "thickness");
        layer.n_elem_min = static_cast<int>(json_number(obj, "n_elem_min"));
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
    try {
        expect(argc == 2, "usage: preview_zalign <chipcae-preview>");
        const fs::path root(CHIPCAE_SOURCE_DIR);
        const auto fixture = root / "schema/examples/fcbga_demo.stack.json";
        const auto thresholds = root / "mesh/background/thresholds/k1.json";
        const double interface_relative = json_number(read_text(thresholds), "interface_relative");
        const double volume_relative = json_number(read_text(thresholds), "volume_relative");
        auto layers = parse_layers(read_text(fixture));
        expect(layers.size() == 3, "fcbga_demo has 3 layers");

        auto dir = fs::temp_directory_path() / ("preview-zalign-" + std::to_string(getpid()));
        fs::create_directories(dir);
        const auto out_prefix = dir / "grid";
        const auto grid = fs::path(out_prefix.string() + ".vtkhdf");
        std::ostringstream cmd;
        cmd << "'" << argv[1] << "' '" << fixture.string() << "' --nx 4 --ny 4 -o '"
            << out_prefix.string() << "' >'" << (dir / "stdout").string() << "' 2>'"
            << (dir / "stderr").string() << "'";
        int code = std::system(cmd.str().c_str());
        expect(WIFEXITED(code) && WEXITSTATUS(code) == 0, "chipcae-preview exit 0");
        expect(fs::exists(grid), "grid.vtkhdf exists");

        hid_t file = H5Fopen(grid.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
        expect(file >= 0, "open vtkhdf");
        auto ncells_v = h5_read<std::int64_t>(file, "VTKHDF/NumberOfCells", H5T_NATIVE_INT64);
        expect(ncells_v.size() == 1, "NumberOfCells scalar");
        const auto nc = static_cast<std::size_t>(ncells_v[0]);
        // Require six z element layers. Do not freeze a particular padded in-plane
        // cell count; accept any square xy grid the preview CLI emits.
        expect(nc % 6 == 0, "cell count divisible by nz=6");
        const auto nxy =
            static_cast<std::size_t>(std::llround(std::sqrt(static_cast<double>(nc / 6))));
        expect(nxy * nxy * 6 == nc, "in-plane cell counts form a square times nz=6");
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
        // Dataset paths and 1-based IDs are the documented preview VTKHDF contract
        // in preview/README.md (CellData layer_id/material_id, FieldData names).
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
        const auto id_underfill = material_index("underfill");
        const auto id_silicon = material_index("silicon");
        const auto id_organic = material_index("organic_eq");
        const auto id_sac = material_index("sac305_ball_eq");

        std::vector<double> z_planes, z_centers;
        double xmin = 1e300, xmax = -1e300, ymin = 1e300, ymax = -1e300;
        for (std::size_t i = 0; i < points.size() / 3; ++i) {
            xmin = std::min(xmin, points[3 * i]);
            xmax = std::max(xmax, points[3 * i]);
            ymin = std::min(ymin, points[3 * i + 1]);
            ymax = std::max(ymax, points[3 * i + 1]);
            z_planes.push_back(points[3 * i + 2]);
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
        std::vector<double> interfaces{z_planes.front()}; // bottom
        // Interfaces are cumulative thicknesses measured from the mesh bottom.
        double zacc = z_planes.front();
        for (const auto& layer : layers) {
            zacc += layer.thickness;
            interfaces.push_back(zacc);
        }
        expect(std::abs(interfaces.back() - z_planes.front() - H) <= iface_tol, "top = bottom+H");
        for (double z : interfaces) {
            auto it = std::find_if(z_planes.begin(), z_planes.end(), [&](double p) {
                return std::abs(p - z) <= iface_tol;
            });
            expect(it != z_planes.end(), "interface z falls on a mesh plane");
        }

        std::vector<int> z_layers_per_id(layers.size(), 0);
        std::vector<std::vector<double>> zc_by_layer(layers.size());
        double volume = 0;
        const double die[4] = {0.005, 0.005, 0.015, 0.015};
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
            if (layers[li].name == "substrate")
                expect(material_id[c] == id_organic, "substrate material");
            else if (layers[li].name == "bump_eq")
                expect(material_id[c] == id_sac, "bump_eq material");
            else if (layers[li].name == "die_layer") {
                const bool in_die = cx >= die[0] && cy >= die[1] && cx <= die[2] && cy <= die[3];
                expect(material_id[c] == (in_die ? id_silicon : id_underfill),
                       "die_layer region material");
            }
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
        const double area = (xmax - xmin) * (ymax - ymin);
        const double expected = area * H;
        expect(std::abs(volume - expected) <= volume_relative * expected, "volume conservation");

        std::cout << "preview_zalign: cells=" << nc << " planes_z=" << z_planes.size()
                  << " volume=" << volume << " expected=" << expected << " ok\n";
        fs::remove_all(dir);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "preview_zalign failed: " << ex.what() << "\n";
        return 1;
    }
}
