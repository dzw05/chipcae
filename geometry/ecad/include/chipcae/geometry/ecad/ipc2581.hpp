#pragma once

#include <array>
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
};
struct UnsupportedElement {
    std::string code = "IMPORT_UNSUPPORTED_ELEMENT";
    std::string element;
    std::uint64_t line = 0;
};
struct ImportReport {
    std::vector<UnsupportedElement> unsupported;
};
struct EcadModel {
    std::string id = "ipc2581";
    double dbu_m = 1e-6;
    std::vector<EcadLayer> layers;
    Polygon outline;
    ImportReport report;
};
struct ImportResult {
    EcadModel model;
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
};

// Parses IPC-2581 with Expat's streaming interface; XML content is never retained as a tree.
ImportResult read_ipc2581(const std::filesystem::path& file);
// Emits stack/0.2 JSON and its binary polyset sidecar.
bool to_stack(const EcadModel& model,
              const std::filesystem::path& json_file,
              const std::filesystem::path& polyset_file,
              std::string* error = nullptr);
} // namespace chipcae::geometry::ecad
