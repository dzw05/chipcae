#pragma once
#include "chipcae/core/types.hpp"

#include <array>
#include <filesystem>
#include <map>
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
struct Layer {
    std::string name, kind, material, body;
    Real thickness = 0, grading = 1;
    Local n_elem_min = 1;
    std::vector<Inclusion> regions;
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
} // namespace chipcae::mesh
