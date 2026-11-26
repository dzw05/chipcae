#ifdef CHIPCAE_ENABLE_KOKKOS
#include <Kokkos_Core.hpp>
#endif
#include "chipcae/core/error_code.hpp"
#include "chipcae/core/parse_floating.hpp"
#include "chipcae/core/version.hpp"
#include "data_path.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef CHIPCAE_PREVIEW_ENABLED
#include "chipcae/mesh/background/column_hierarchy.hpp"
#include "chipcae/mesh/background/copper_map.hpp"
#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/background/vtkhdf_writer.hpp"
#include "chipcae/mesh/cut/detail/outline_nonconvex.hpp"
#include "chipcae/mesh/cut/outline_cut.hpp"
#include "chipcae/mesh/cut/region_cut.hpp"
#include "mesh_digest.hpp"
#include "mesh_error_map.hpp"
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif
#endif

namespace {
std::string quote(const std::string& text) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < text.size(); ++i) {
        auto c = static_cast<unsigned char>(text[i]);
        if (c >= 128) {
            std::size_t length = c >= 0xc2 && c <= 0xdf   ? 2
                                 : c >= 0xe0 && c <= 0xef ? 3
                                 : c >= 0xf0 && c <= 0xf4 ? 4
                                                          : 0;
            bool valid = length && i + length <= text.size();
            for (std::size_t j = 1; valid && j < length; ++j) {
                auto next = static_cast<unsigned char>(text[i + j]);
                valid = next >= 0x80 && next <= 0xbf;
                if (j == 1)
                    valid = valid && !(c == 0xe0 && next < 0xa0) && !(c == 0xed && next >= 0xa0) &&
                            !(c == 0xf0 && next < 0x90) && !(c == 0xf4 && next >= 0x90);
            }
            if (valid) {
                result.append(text, i, length);
                i += length - 1;
            } else {
                result += "\\ufffd";
            }
            continue;
        }
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 32) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}
void event(const std::string& stage, double progress, const std::string& message) {
#if defined(CHIPCAE_PREVIEW_ENABLED) && defined(CHIPCAE_ENABLE_MPI)
    int initialized = 0, rank = 0;
    MPI_Initialized(&initialized);
    if (initialized)
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    if (rank != 0 && stage != "error")
        return;
#endif
    if (stage == "success" && message.size() >= 2 && message.front() == '{' &&
        message.back() == '}')
        std::cout << "{\"stage\":" << quote(stage) << ",\"progress\":" << progress << ','
                  << message.substr(1, message.size() - 2) << ",\"message\":" << quote(message)
                  << "}\n"
                  << std::flush;
    else
        std::cout << "{\"stage\":" << quote(stage) << ",\"progress\":" << progress
                  << ",\"message\":" << quote(message) << "}\n"
                  << std::flush;
}
#ifdef CHIPCAE_PREVIEW_ENABLED
using chipcae::preview::detail::digest;
#endif
#ifdef CHIPCAE_PREVIEW_ENABLED
int integer(const std::string& value, bool zero) {
    int n = 0;
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (error != std::errc{} || end != value.data() + value.size() || n < (zero ? 0 : 1))
        throw std::runtime_error("invalid integer: " + value);
    return n;
}
struct MeshFailure : std::runtime_error {
    chipcae::mesh::Error error;
    explicit MeshFailure(const chipcae::mesh::Error& value)
        : std::runtime_error(value.path + ": " + value.reason), error(value) {}
};
int exit_code(const chipcae::mesh::Error& error) {
    return chipcae::solve::mesh_error_code(error).code();
}
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (std::filesystem::weakly_canonical(a) == std::filesystem::weakly_canonical(b))
        return true;
    std::error_code error;
    return std::filesystem::equivalent(a, b, error);
}
template <class T>
T checked(chipcae::mesh::Expected<T> result) {
    if (!result)
        throw MeshFailure(result.error());
    return std::move(result.value());
}
template <class T>
T checked_io(chipcae::mesh::Expected<T> result) {
    if (!result)
        throw std::runtime_error(result.error().path + ": " + result.error().reason);
    return std::move(result.value());
}
struct CopperColumnAreas {
    chipcae::Real background = 0;
    chipcae::Real raster = 0;
};
CopperColumnAreas copper_column_areas(const chipcae::mesh::HexMeshArrays& mesh, std::size_t li) {
    using chipcae::mesh::level0_dense;
    const auto& x = mesh.axes[0];
    const auto& y = mesh.axes[1];
    const std::size_t nx = x.size() - 1, ny = y.size() - 1;
    const std::size_t offset = static_cast<std::size_t>(mesh.copper_layer_of_layer[li]) * nx * ny;
    CopperColumnAreas result;
    for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const auto c = level0_dense(static_cast<std::uint32_t>(nx),
                                        static_cast<std::uint32_t>(ny),
                                        static_cast<std::uint32_t>(i),
                                        static_cast<std::uint32_t>(j));
            const chipcae::Real area = (x[i + 1] - x[i]) * (y[j + 1] - y[j]);
            result.background += area;
            result.raster += mesh.copper_phi[offset + c] * area;
        }
    return result;
}
int run(int argc, char** argv) {
    using namespace chipcae::mesh;
    std::filesystem::path input, output, surface, polyset;
    GridOptions options;
    std::optional<int> min_elems_per_layer;
    bool nonconvex_outline = false;
#ifdef CHIPCAE_ENABLE_MPI
    int world_size = 1;
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);
    options.ranks = world_size;
#endif
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "--surface" || arg == "--polyset" || arg == "--nx" ||
            arg == "--ny" || arg == "--levels" || arg == "--max-ratio" || arg == "--cell-budget" ||
            arg == "--min-elems-per-layer") {
            if (!seen.insert(arg).second)
                throw std::runtime_error("duplicate option: " + arg);
            if (++i == argc || std::string(argv[i]).empty())
                throw std::runtime_error("missing value for " + arg);
            std::string value = argv[i];
            if (arg == "-o")
                output = value;
            else if (arg == "--surface")
                surface = value;
            else if (arg == "--polyset")
                polyset = value;
            else if (arg == "--nx")
                options.nx = integer(value, false);
            else if (arg == "--ny")
                options.ny = integer(value, false);
            else if (arg == "--cell-budget")
                options.cell_budget = integer(value, false);
            else if (arg == "--min-elems-per-layer")
                min_elems_per_layer = integer(value, false);
            else if (arg == "--max-ratio") {
                double ratio = 0;
                if (!chipcae::parse_floating(value, ratio) || !std::isfinite(ratio) || ratio < 1)
                    throw std::runtime_error("invalid max ratio: " + value);
                options.max_ratio = ratio;
            } else
                options.levels = integer(value, true);
        } else if (arg == "--nonconvex-outline") {
            if (!seen.insert(arg).second)
                throw std::runtime_error("duplicate option: " + arg);
            nonconvex_outline = true;
        } else if (arg.starts_with('-') || !input.empty()) {
            throw std::runtime_error("unexpected argument: " + arg);
        } else {
            input = arg;
        }
    }
    if (input.empty() || output.empty())
        throw std::runtime_error("input and -o are required; use --help for usage");
    if (seen.contains("--nx") != seen.contains("--ny"))
        throw std::runtime_error("--nx and --ny must be supplied together");
    auto stem = output;
    if (stem.extension() == ".vtkhdf" || stem.filename() == ".vtkhdf")
        stem = stem.parent_path() /
               stem.filename().string().substr(0, stem.filename().string().size() - 7);
    auto grid_path = std::filesystem::path(stem.string() + ".vtkhdf");
    if (!surface.empty() && same_path(surface, grid_path))
        throw std::runtime_error("surface and grid paths must differ");
    event("load", 0, "Loading stack 0.2");
    auto stack = checked_io(load_stack(input, chipcae::data_path(CHIPCAE_PREVIEW_SCHEMA)));
    if (min_elems_per_layer)
        for (auto& layer : stack.layers)
            layer.n_elem_min = std::max(layer.n_elem_min, *min_elems_per_layer);
    for (const auto& warning : stack.warnings)
        std::cerr << warning << '\n';
    std::set<std::filesystem::path> sidecars;
    if (!polyset.empty())
        sidecars.insert(polyset);
    else
        for (const auto& layer : stack.layers)
            if (layer.copper && layer.copper->polygons)
                sidecars.insert(input.parent_path() / layer.copper->polygons->file);
    Polyset polygons;
    std::set<std::string> polygon_names;
    for (const auto& path : sidecars) {
        auto part = checked_io(read_polyset(path));
        if (polygons.dbu_m != 0 && polygons.dbu_m != part.dbu_m)
            throw std::runtime_error("polysets must use the same database unit");
        polygons.dbu_m = part.dbu_m;
        for (auto& layer : part.layers) {
            if (!polygon_names.insert(layer.name).second)
                throw std::runtime_error("duplicate polyset layer name: " + layer.name);
            polygons.layers.push_back(std::move(layer));
        }
    }
    if (stack.outline.polygon.empty() && polygon_names.contains("outline"))
        throw MeshFailure(
            {"outline.unsupported",
             "external polyset outline is not supported; use an inline outline polygon"});
    auto bounding_stack = stack;
    bounding_stack.outline.polygon.clear();
    event("mesh", 0.3, "Building tensor grid");
    auto mesh = sidecars.empty() ? checked(build_tensor_grid(bounding_stack, options))
                                 : checked(build_tensor_grid(bounding_stack, options, polygons));
    auto region_planes = checked(cut::region_cut_planes(stack, mesh));
    auto cut =
        checked(nonconvex_outline
                    ? cut::detail::apply_outline_cut_general(mesh, stack.outline, {}, region_planes)
                    : cut::apply_outline_cut(mesh, stack.outline, {}, region_planes));
    event("write", 0.8, "Writing VTKHDF");
    auto files =
        checked_io(write_vtkhdf(stem, mesh, cut ? &*cut : nullptr, chipcae::Communicator::world()));
    int rank = 0;
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
#endif
    int rename_failed = 0;
    if (rank == 0 && !surface.empty()) {
        try {
            if (!same_path(surface, files.embedded_boundary))
                std::filesystem::rename(files.embedded_boundary, surface);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            rename_failed = 1;
        }
    }
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Bcast(&rename_failed, 1, MPI_INT, 0, MPI_COMM_WORLD);
#endif
    if (rename_failed)
        throw std::runtime_error("could not move embedded boundary to requested surface path");
    const auto& report = mesh.grid_report;
    const auto& coarse = report.level_axes.front();
    const auto summary = digest(mesh);
    std::ostringstream effective;
    effective << std::setprecision(17) << "{\"grid\":" << quote(files.grid.string())
              << ",\"max_ratio\":" << options.max_ratio << ",\"n0\":"
              << quote(std::to_string(coarse[0].size() - 1) + "x" +
                       std::to_string(coarse[1].size() - 1))
              << ",\"nx0\":" << summary.nx0 << ",\"ny0\":" << summary.ny0
              << ",\"L\":" << summary.levels << ",\"padded_cells\":" << summary.padded_cells
              << ",\"void_cells\":" << summary.void_cells << ",\"nz\":" << summary.nz
              << ",\"node_count\":" << summary.nodes << ",\"cell_count\":" << summary.cells
              << ",\"cut_cell_count\":" << (cut ? cut->cut_elem.size() : 0)
              << ",\"mesh_sha256\":" << quote(summary.sha256)
              << ",\"cell_budget\":" << options.cell_budget << ",\"min_elems_per_layer\":"
              << (min_elems_per_layer ? std::to_string(*min_elems_per_layer) : "null")
              << ",\"copper_area_m2\":{";
    bool first_copper = true;
    for (std::size_t li = 0; li < stack.layers.size(); ++li) {
        const auto& layer = stack.layers[li];
        if (!layer.copper)
            continue;
        auto [background_area, raster_area] = copper_column_areas(mesh, li);
        chipcae::Real polygon_area = 0;
        if (layer.copper->fraction) {
            // A shared constant-field reduction keeps both values bitwise identical.
            polygon_area = raster_area = *layer.copper->fraction * background_area;
        } else if (layer.copper->polygons) {
            const auto source =
                std::find_if(polygons.layers.begin(), polygons.layers.end(), [&](const auto& p) {
                    return p.name == layer.copper->polygons->layer;
                });
            polygon_area = copper_union_area_m2(*source, polygons.dbu_m);
        }
        if (!first_copper)
            effective << ',';
        first_copper = false;
        effective << quote(layer.name) << ":{\"polygons\":" << polygon_area
                  << ",\"raster\":" << raster_area << '}';
    }
    effective << "}}";
    event("success", 1, effective.str());
    return 0;
}
#endif
} // namespace
int main(int argc, char** argv) {
    if (argc == 2) {
        std::string arg = argv[1];
        if (arg == "--help" || arg == "-h") {
            event("help",
                  1,
                  "chipcae-preview <stack.json> [--polyset f] [--nx N --ny N] "
                  "[--levels N] [--max-ratio R] [--cell-budget B] [--min-elems-per-layer M] "
                  "[--nonconvex-outline] "
                  "-o <stem|out.vtkhdf> [--surface <exact-eb-file>] "
                  "(--polyset overrides meshing sidecars; levels=0 is automatic)");
            return 0;
        }
        if (arg == "--version" || arg == "-v") {
            event("version", 1, std::string(chipcae::version_string()));
            return 0;
        }
    }
#ifdef CHIPCAE_PREVIEW_ENABLED
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Init(&argc, &argv);
#endif
    int code = 0;
    try {
#ifdef CHIPCAE_ENABLE_KOKKOS
        Kokkos::ScopeGuard guard;
#endif
        code = run(argc, argv);
    } catch (const MeshFailure& error) {
        std::cerr << error.what() << '\n';
        event("error", 1, error.what());
        code = exit_code(error.error);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        event("error", 1, error.what());
        code = chipcae::ErrorCode::INTERNAL;
    }
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Finalize();
#endif
    return code;
#else
    std::cerr << "preview requires JSONCONS and HDF5 support\n";
    event("error", 1, "preview requires JSONCONS and HDF5 support");
    return chipcae::ErrorCode::NOT_IMPLEMENTED;
#endif
}
