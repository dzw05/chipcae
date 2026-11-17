#include "map.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace chipcae::geometry::ecad::odbpp_detail {
std::string fold_ascii(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c = char(c + ('a' - 'A'));
    return s;
}
double unit_m(const OdbJob& j) {
    return j.units == detail::OdbUnits::inch ? 0.0254 : 0.001;
}
void map_stackup(const OdbJob& j, const MapOptions& options, ImportResult& result) {
    if (j.layers.empty())
        throw std::runtime_error("empty matrix layer list");
    std::set<std::string> names;
    for (const auto& l : j.layers) {
        if (!names.insert(l.name).second)
            throw std::runtime_error("duplicate layer: " + l.name);
        std::optional<std::string> matched;
        if (options.overrides)
            for (const auto& [key, value] : *options.overrides) {
                (void)value;
                if (fold_ascii(key) != fold_ascii(l.name))
                    continue;
                if (matched)
                    throw std::runtime_error("override collision: " + *matched + ", " + key);
                matched = key;
            }
    }
    auto layers = j.layers;
    std::stable_sort(
        layers.begin(), layers.end(), [](const auto& a, const auto& b) { return a.row < b.row; });
    std::vector<std::string> missing;
    for (const auto& l : layers) {
        const auto type = fold_ascii(l.type);
        const bool copper = type == "signal" || type == "power_ground";
        if (!copper && type != "dielectric") {
            if (type != "drill")
                result.model.report.unsupported.push_back(
                    {"IMPORT_UNSUPPORTED_ELEMENT",
                     type == "solder_mask" ? "SolderMask:" + l.name
                                           : "SkippedLayer:" + l.name + ":" + l.type,
                     0});
            continue;
        }
        EcadLayer layer;
        layer.name = l.name;
        layer.function = type;
        layer.copper = copper;
        layer.material = copper ? "copper" : "";
        double odb = 0;
        auto attrs = j.attrs.layer.find(l.name);
        if (attrs != j.attrs.layer.end()) {
            auto material = attrs->second.find(".material");
            if (material != attrs->second.end() && !material->second.empty())
                layer.material = material->second;
            auto thickness = attrs->second.find(".layer_dielectric");
            if (thickness != attrs->second.end()) {
                try {
                    std::size_t used = 0;
                    const auto parsed = std::stold(thickness->second, &used) *
                                        (j.units == detail::OdbUnits::inch ? 0.0254L : 0.001L);
                    odb = static_cast<double>(parsed);
                    if (used != thickness->second.size() || !std::isfinite(odb) || odb <= 0)
                        odb = 0;
                } catch (const std::exception&) {
                    odb = 0;
                }
            }
        }
        if (!copper && layer.material.empty())
            throw std::runtime_error("missing .material for dielectric layer: " + l.name);
        double selected = odb;
        std::string source = odb > 0 ? "odb" : "missing";
        std::optional<double> original;
        if (options.overrides)
            for (const auto& [key, value] : *options.overrides) {
                if (fold_ascii(key) != fold_ascii(l.name))
                    continue;
                if (!std::isfinite(value) || value <= 0)
                    throw std::runtime_error("invalid override thickness: " + key);
                if (odb > 0 && std::abs(value - odb) <= 1e-9 * odb)
                    continue;
                selected = value;
                source = "override";
                if (odb > 0)
                    original = odb;
            }
        if (selected <= 0)
            missing.push_back(l.name);
        ThicknessRecord record{l.name, selected, source, original};
        if (options.records)
            options.records->push_back(record);
        layer.thickness_m = selected;
        result.model.layers.push_back(std::move(layer));
    }
    if (!missing.empty()) {
        std::string error =
            "missing thickness for " + std::to_string(missing.size()) + " layer(s): ";
        for (const auto& name : missing)
            error += name + ", ";
        throw std::runtime_error(error.substr(0, error.size() - 2));
    }
}
} // namespace chipcae::geometry::ecad::odbpp_detail
