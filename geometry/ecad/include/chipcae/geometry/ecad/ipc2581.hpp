#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace chipcae::geometry::ecad {
struct Polygon {
    std::vector<std::array<std::int64_t, 2>> points;
};
struct EcadLayer {
    std::string name;
    std::string material;
    std::string function;
    double thickness_m = 0;
    bool copper = false;
    std::vector<Polygon> polygons;
    std::string metal{}; // Mapped copper metal ID; material stores the base dielectric ID.
};
struct UnsupportedElement {
    std::string code = "IMPORT_UNSUPPORTED_ELEMENT";
    std::string element;
    std::uint64_t line = 0;
};
struct ImportReport {
    std::vector<UnsupportedElement> unsupported;
};
// Circular drill diameter is the outer conductor diameter. A referenced material
// Spec/General/Property named PLATING_THICKNESS supplies radial thickness (with unit).
// Thickness >= radius denotes solid fill; absent thickness is reported unsupported.
struct Via {
    std::string id, padstack, start_layer, end_layer;
    std::array<std::int64_t, 2> position{};
    double outer_radius_dbu = 0, inner_radius_dbu = 0;
    std::vector<std::size_t> dielectric_layers;
};
struct ComponentPad {
    std::string id;
    std::array<std::int64_t, 2> position{};
};
struct Component {
    std::string id, package;
    std::array<std::int64_t, 2> position{};
    double rotation_degrees = 0;
    bool mirror = false;
    Polygon outline;
    std::vector<ComponentPad> pads;
};
struct ViaFraction {
    std::size_t layer = 0;
    // Cell bounds in dbu; fraction is unioned conductor area / rectangular cell area.
    std::array<double, 2> lower{}, upper{};
    double fraction = 0;
    std::size_t via_count = 0;
};
struct EcadModel {
    std::string id = "ipc2581";
    double dbu_m = 1e-6;
    std::vector<EcadLayer> layers;
    Polygon outline;
    ImportReport report;
    std::vector<Via> vias;
    std::vector<Component> components;
    // Import initializes one bounding cell per crossed dielectric layer.
    std::vector<ViaFraction> via_fractions;
};
struct ImportResult {
    EcadModel model;
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};

// Loads a JSON object mapping ECAD material names to material library IDs and
// applies it to every layer. Missing entries are reported as MATERIAL_MAPPING_MISSING.
bool apply_material_map(EcadModel& model,
                        const std::filesystem::path& file,
                        std::string* error = nullptr);

// Parses IPC-2581 with Expat's streaming interface; XML content is never retained as a tree.
ImportResult read_ipc2581(const std::filesystem::path& file);
// Recompute via fractions on caller-supplied rectangular cells (dbu).
// Copper layers are rejected. Overlapping conductors are unioned, including annular holes.
bool rasterize_vias(const EcadModel& model, std::vector<ViaFraction>& cells);
// Emits stack/0.2 JSON and its binary polyset sidecar.
bool to_stack(const EcadModel& model,
              const std::filesystem::path& json_file,
              const std::filesystem::path& polyset_file,
              std::string* error = nullptr);
} // namespace chipcae::geometry::ecad
