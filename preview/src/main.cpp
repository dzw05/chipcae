#include "chipcae/core/error_code.hpp"
#include "chipcae/core/version.hpp"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

#ifdef CHIPCAE_PREVIEW_ENABLED
#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/background/vtkhdf_writer.hpp"
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif
#endif

namespace {
std::string quote(const std::string& text) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
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
    std::cout << "{\"stage\":" << quote(stage) << ",\"progress\":" << progress
              << ",\"message\":" << quote(message) << "}\n"
              << std::flush;
}
#ifdef CHIPCAE_PREVIEW_ENABLED
int integer(const std::string& value, bool zero) {
    int n = 0;
    auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (error != std::errc{} || end != value.data() + value.size() || n < (zero ? 0 : 1))
        throw std::runtime_error("invalid integer: " + value);
    return n;
}
template <class T>
T checked(chipcae::mesh::Expected<T> result) {
    if (!result)
        throw std::runtime_error(result.error().path + ": " + result.error().reason);
    return std::move(result.value());
}
int run(int argc, char** argv) {
    using namespace chipcae::mesh;
    std::filesystem::path input, output, surface, polyset;
    GridOptions options;
    // The K1 FCBGA fixture has a 10:1 interface cell-height ratio.
    options.max_ratio = 10;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" || arg == "--surface" || arg == "--polyset" || arg == "--nx" ||
            arg == "--ny" || arg == "--levels") {
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
            else
                options.levels = integer(value, true);
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
    if (stem.extension() == ".vtkhdf")
        stem.replace_extension();
    auto grid_path = std::filesystem::path(stem.string() + ".vtkhdf");
    if (!surface.empty() && std::filesystem::absolute(surface).lexically_normal() ==
                                std::filesystem::absolute(grid_path).lexically_normal())
        throw std::runtime_error("surface and grid paths must differ");
    event("load", 0, "Loading stack 0.2");
    auto stack = checked(load_stack(input, CHIPCAE_PREVIEW_SCHEMA));
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
    for (const auto& path : sidecars) {
        auto part = checked(read_polyset(path));
        if (polygons.dbu_m != 0 && polygons.dbu_m != part.dbu_m)
            throw std::runtime_error("polysets must use the same database unit");
        polygons.dbu_m = part.dbu_m;
        for (auto& layer : part.layers)
            polygons.layers.push_back(std::move(layer));
    }
    event("mesh", 0.3, "Building tensor grid");
    auto mesh = sidecars.empty() ? checked(build_tensor_grid(stack, options))
                                 : checked(build_tensor_grid(stack, options, polygons));
    event("write", 0.8, "Writing VTKHDF");
    auto files = checked(write_vtkhdf(stem, mesh, nullptr, chipcae::Communicator::world()));
    if (!surface.empty() &&
        std::filesystem::absolute(surface).lexically_normal() !=
            std::filesystem::absolute(files.embedded_boundary).lexically_normal())
        std::filesystem::rename(files.embedded_boundary, surface);
    event("success", 1, files.grid.string());
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
                  "[--levels N] -o <stem|out.vtkhdf> [--surface <exact-eb-file>] "
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
        code = run(argc, argv);
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
