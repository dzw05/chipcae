#pragma once
#include "chipcae/core/types.hpp"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>
namespace chipcae::mesh {
struct Error {
    std::string path, reason;
};
template <class T>
class Expected {
    std::variant<T, Error> data_;

public:
    Expected(T value) : data_(std::move(value)) {}
    Expected(Error error) : data_(std::move(error)) {}
    explicit operator bool() const { return std::holds_alternative<T>(data_); }
    T& value() { return std::get<T>(data_); }
    const T& value() const { return std::get<T>(data_); }
    const Error& error() const { return std::get<Error>(data_); }
};
using Local = std::int32_t;
struct Footprint {
    std::array<Real, 4> box{};
    std::vector<std::array<Real, 2>> polygon;
};
struct Inclusion {
    std::string name, material, body;
    Footprint footprint;
};
struct CopperPolygons {
    std::string file, layer;
};
struct CopperSpec {
    std::string metal;
    std::optional<Real> fraction;
    std::optional<CopperPolygons> polygons;
    // Test-first interface stubs; parsing and serialization are not implemented yet.
    std::string orientation = "planar";
    std::string role = "metal";
};
struct Layer {
    std::string name, kind, material, body;
    Real thickness = 0, grading = 1;
    Local n_elem_min = 1;
    std::vector<Inclusion> regions;
    std::optional<CopperSpec> copper;
};
struct StackModel {
    std::string id, unit = "m", symmetry, package_type;
    std::vector<std::string> warnings;
    Footprint outline;
    std::vector<Layer> layers;
    std::map<std::string, std::vector<std::string>> activation_groups;
};
Expected<StackModel> load_stack(const std::filesystem::path& instance,
                                const std::filesystem::path& schema);

struct PolysetLayer {
    std::string name;
    // Offsets index points. The final offset is the number of points; each point has x,y.
    std::vector<std::int64_t> ring_offsets;
    std::vector<std::int64_t> coordinates;
};
struct Polyset {
    double dbu_m = 0;
    std::vector<PolysetLayer> layers;
};
Expected<Polyset> read_polyset(const std::filesystem::path& file);
Expected<bool> write_polyset(const std::filesystem::path& file, const Polyset& polyset);
} // namespace chipcae::mesh
