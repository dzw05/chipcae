#include "chipcae/geometry/ecad/odbpp.hpp"
#include "map.hpp"
#include "parser.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <set>

namespace chipcae::geometry::ecad {
ImportResult read_odbpp(const std::filesystem::path& path) {
    return read_odbpp(path, OdbReadOptions{});
}

ImportResult read_odbpp(const std::filesystem::path& path,
                        const OdbReadOptions& options,
                        std::vector<ThicknessRecord>* records) {
    if (records)
        records->clear();
    try {
        detail::OdbJob job;
        auto result = detail::read_odbpp_job(path, job);
        if (!result)
            return result;
        odbpp_detail::ThicknessOverrides overrides;
        std::set<std::string> names;
        for (const auto& [name, value] : options.overrides) {
            if (name.empty() || !std::isfinite(value) || value <= 0 ||
                !names.insert(odbpp_detail::fold_ascii(name)).second) {
                result.error = "IMPORT_REJECTED: invalid or duplicate thickness override: " + name;
                return result;
            }
            overrides.emplace(name, value);
        }
        std::vector<odbpp_detail::ThicknessRecord> mapped_records;
        auto mapped = odbpp_detail::map_odb(job, {&overrides, &mapped_records});
        if (!mapped) {
            result.error = mapped.error().message;
            return result;
        }
        // The frozen mapper uses overrides only for missing thickness. Enforce
        // the public override-wins rule here; via fractions depend on XY area,
        // so replacing a layer thickness does not invalidate mapped geometry.
        for (auto& record : mapped_records) {
            const auto match = std::find_if(overrides.begin(), overrides.end(), [&](const auto& e) {
                return odbpp_detail::fold_ascii(e.first) == odbpp_detail::fold_ascii(record.layer);
            });
            if (match == overrides.end())
                continue;
            record.odb_value_m.reset();
            if (record.source == "odb" && std::abs(record.thickness_m - match->second) > 0)
                record.odb_value_m = record.thickness_m;
            record.thickness_m = match->second;
            record.source = "override";
        }
        for (auto& layer : mapped.value().layers) {
            const auto match = std::find_if(overrides.begin(), overrides.end(), [&](const auto& e) {
                return odbpp_detail::fold_ascii(e.first) == odbpp_detail::fold_ascii(layer.name);
            });
            if (match != overrides.end())
                layer.thickness_m = match->second;
        }
        auto unsupported = std::move(result.model.report.unsupported);
        result.model = std::move(mapped.value());
        result.model.report.unsupported.insert(
            result.model.report.unsupported.begin(), unsupported.begin(), unsupported.end());

        // The frozen parser normalizes TYPE. Recover its source spelling for reports
        // without changing parser or mapper contracts. ArchiveReader owns all access.
        detail::ArchiveReader reader(path);
        std::map<std::string, std::string> functions;
        std::string name, function, report_error;
        bool in_layer = false;
        reader.for_each_line(
            "matrix/matrix",
            [&](std::string_view raw) {
                const auto line = detail::trim(raw);
                if (line.starts_with("LAYER") && line.find('{') != std::string::npos) {
                    in_layer = true;
                    name.clear();
                    function.clear();
                } else if (line == "}") {
                    if (in_layer)
                        functions.emplace(name, function);
                    in_layer = false;
                } else if (in_layer) {
                    const auto eq = line.find('=');
                    if (eq != std::string::npos) {
                        const auto key = detail::trim(std::string_view(line).substr(0, eq));
                        const auto value = detail::trim(std::string_view(line).substr(eq + 1));
                        if (key == "NAME")
                            name = value;
                        else if (key == "TYPE")
                            function = value;
                    }
                }
                return true;
            },
            report_error);
        // Source spelling is best-effort report metadata. A missing entry or
        // failed second read must preserve the function already mapped above.
        for (auto& layer : result.model.layers) {
            const auto found = functions.find(layer.name);
            if (found != functions.end() && !found->second.empty())
                layer.function = found->second;
        }
        for (auto& entry : result.model.report.unsupported) {
            if (!entry.element.starts_with("SkippedLayer:"))
                continue;
            const auto end = entry.element.rfind(':');
            const auto layer = entry.element.substr(13, end - 13);
            const auto found = functions.find(layer);
            if (found != functions.end() && !found->second.empty())
                entry.element = "SkippedLayer:" + layer + ":" + found->second;
        }
        if (records)
            for (const auto& record : mapped_records)
                records->push_back(
                    {record.layer, record.thickness_m, record.source, record.odb_value_m});
        return result;
    } catch (const std::exception& error) {
        if (records)
            records->clear();
        ImportResult result;
        result.error = "IMPORT_REJECTED: " + std::string(error.what());
        return result;
    }
}
} // namespace chipcae::geometry::ecad
