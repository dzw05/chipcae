#include "chipcae/geometry/import_report.hpp"
#include "import_common.hpp"
#include "stack_override.hpp"
#ifdef CHIPCAE_ENABLE_ECAD
#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "chipcae/geometry/ecad/odbpp.hpp"
#include "chipcae/geometry/import_accept.hpp"
#include "import_ecad_defects.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace chipcae::importer {
#ifdef CHIPCAE_ENABLE_ECAD
namespace {
using ecad_defects::classify_unsupported;
using ecad_defects::SkippedRow;
namespace ecad = chipcae::geometry::ecad;
using chipcae::geometry::Defect;
using chipcae::geometry::Tier;
std::string json_string(const std::string& value) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 0x20) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    return out + '"';
}
bool odb_input(const std::filesystem::path& input) {
    const auto name = fold_layer_name(input.filename().string());
    if (name.ends_with(".tgz") || name.ends_with(".tar.gz") || name.ends_with(".zip"))
        return true;
    std::error_code error;
    return std::filesystem::is_directory(input, error) &&
           std::filesystem::is_regular_file(input / "matrix/matrix", error);
}

bool write_layers(const std::filesystem::path& file,
                  const ecad::EcadModel& model,
                  const std::vector<LayerSourceRow>& rows,
                  const std::vector<SkippedRow>& skipped,
                  const std::vector<ecad::ThicknessRecord>* records,
                  const std::optional<std::string>& source,
                  const std::vector<std::string>& warnings) {
    std::ofstream out(file);
    out << std::setprecision(17) << '{';
    if (source)
        out << "\"source\":" << json_string(*source) << ',';
    out << "\"warnings\":[";
    for (std::size_t i = 0; i < warnings.size(); ++i)
        out << (i ? "," : "") << json_string(warnings[i]);
    out << "],\"layers\":[";
    bool first = true;
    for (std::size_t i = 0; i < model.layers.size(); ++i) {
        const auto& layer = model.layers[i];
        const auto& row = rows[i];
        if (!first)
            out << ',';
        first = false;
        out << "{\"name\":" << json_string(layer.name)
            << ",\"status\":\"kept\",\"function\":" << json_string(layer.function)
            << ",\"copper\":" << (layer.copper ? "true" : "false")
            << ",\"material_key\":" << json_string(row.material_key)
            << ",\"material_id\":" << json_string(row.material_id)
            << ",\"thickness_m\":" << layer.thickness_m
            << ",\"thickness_source\":" << json_string(row.thickness_source);
        if (records) {
            const auto record = std::find_if(records->begin(), records->end(), [&](const auto& r) {
                return r.layer == layer.name;
            });
            if (record != records->end() && record->odb_value_m)
                out << ",\"odb_value_m\":" << *record->odb_value_m;
        }
        out << '}';
    }
    for (const auto& row : skipped) {
        if (!first)
            out << ',';
        first = false;
        out << "{\"name\":" << json_string(row.name)
            << ",\"status\":\"skipped\",\"function\":" << json_string(row.function)
            << ",\"copper\":false,\"material_key\":\"\",\"material_id\":\"\"}";
    }
    out << "]}\n";
    out.close();
    return static_cast<bool>(out);
}

// Each output is staged beside its destination (including a custom polyset).
// On publication failure restore previous files; the layer report is published last.
class StagedOutputs {
    struct File {
        std::filesystem::path target, stage, backup;
        bool saved = false, published = false;
    };
    std::vector<File> files_;

public:
    ~StagedOutputs() {
        for (auto& file : files_) {
            std::error_code ignored;
            if (file.saved) {
                if (file.published)
                    std::filesystem::remove(file.target, ignored);
                std::filesystem::rename(file.backup, file.target, ignored);
            } else if (file.published)
                std::filesystem::remove(file.target, ignored);
            std::filesystem::remove_all(file.stage.parent_path(), ignored);
        }
    }
    std::filesystem::path add(const std::filesystem::path& target) {
        auto parent = target.parent_path();
        if (parent.empty())
            parent = ".";
        std::filesystem::create_directories(parent);
        // Atomic directory creation avoids collisions between concurrent importers.
        for (unsigned i = 0; i < 10000; ++i) {
            const auto dir = parent / (".chipcae-import-" + std::to_string(i));
            if (!std::filesystem::create_directory(dir))
                continue;
            const auto stage = dir / target.filename();
            files_.push_back({target, stage, dir / "backup" / target.filename(), false, false});
            return stage;
        }
        throw std::runtime_error("cannot reserve import staging directory");
    }
    void publish() {
        for (auto& file : files_) {
            if (std::filesystem::exists(file.target)) {
                if (!std::filesystem::is_regular_file(file.target))
                    throw std::runtime_error("output destination is not a regular file");
                std::filesystem::create_directory(file.backup.parent_path());
                std::filesystem::rename(file.target, file.backup);
                file.saved = true;
            }
            std::filesystem::rename(file.stage, file.target);
            file.published = true;
        }
        // All publications succeeded; destructor now only cleans staging directories.
        for (auto& file : files_) {
            file.saved = false;
            file.published = false;
        }
    }
};
} // namespace
#endif

int run_ecad(const ImportOptions& options) {
    const auto& report = options.report;
#ifndef CHIPCAE_ENABLE_ECAD
    write_defects(report, "IMPORT_REJECTED", "ECAD support is disabled");
    std::cerr
        << "IMPORT_REJECTED: ECAD support is disabled; configure with CHIPCAE_ENABLE_ECAD=ON\n";
    return CliExit::import_rejected;
#else
    auto reject = [&](const std::string& message, bool missing = false) {
        // The shared summary writer inserts message bytes between JSON quotes.
        const auto encoded = json_string(message);
        write_defects(report,
                      missing ? "MATERIAL_MAPPING_MISSING" : "IMPORT_REJECTED",
                      encoded.substr(1, encoded.size() - 2));
        if (!message.starts_with("IMPORT_") && !message.starts_with("MATERIAL_"))
            std::cerr << (missing ? "MATERIAL_MAPPING_MISSING: " : "IMPORT_REJECTED: ");
        std::cerr << message << '\n';
        return missing ? CliExit::material_mapping_missing : CliExit::import_rejected;
    };
    if (options.accept_b_tier && !options.accept_file.empty())
        return CliExit::usage;
    if (options.material_map.empty())
        return reject("--material-map is required", true);
    try {
        ParsedStackOverride overrides;
        if (!options.stack_override.empty()) {
            auto parsed = read_stack_override(options.stack_override);
            if (!parsed)
                return reject(parsed.error().message);
            overrides = std::move(parsed.value());
        }
        const bool odb = odb_input(options.input);
        std::vector<ecad::ThicknessRecord> records;
        auto imported = odb ? ecad::read_odbpp(options.input, {overrides.thicknesses}, &records)
                            : ecad::read_ipc2581(options.input);
        if (!imported)
            return reject(imported.error);
        auto& model = imported.model;
        if (odb && records.size() != model.layers.size())
            return reject("IMPORT_REJECTED: ODB thickness record count does not match layers");
        // Modeling notices are stderr-only; ordinary warnings also enter the layer report.
        std::vector<std::string> mask_notices;
        if (!odb) {
            std::set<std::string> original_names;
            for (const auto& layer : model.layers)
                original_names.insert(layer.name);
            std::string error;
            if (!ecad::restore_solder_masks(model, options.material_map, &error))
                return reject(error, error.find("MATERIAL_MAPPING_MISSING") != std::string::npos);
            for (const auto& layer : model.layers)
                if (!original_names.contains(layer.name))
                    mask_notices.push_back(
                        "solder mask modeled as full-coverage homogenized layer: " + layer.name);
        }
        std::vector<LayerSourceRow> rows;
        std::vector<std::string> warnings;
        std::vector<std::string> missing_thickness;
        for (std::size_t i = 0; i < model.layers.size(); ++i) {
            auto& layer = model.layers[i];
            if (odb && records[i].layer != layer.name)
                return reject("IMPORT_REJECTED: ODB thickness record does not match layer " +
                              layer.name);
            std::string source = odb ? records[i].source : "ipc";
            if (!odb) {
                const auto match = std::find_if(overrides.thicknesses.begin(),
                                                overrides.thicknesses.end(),
                                                [&](const auto& entry) {
                                                    return fold_layer_name(entry.first) ==
                                                           fold_layer_name(layer.name);
                                                });
                if (match != overrides.thicknesses.end()) {
                    if (std::isfinite(layer.thickness_m) && layer.thickness_m > 0 &&
                        std::abs(layer.thickness_m - match->second) / layer.thickness_m > 1e-9)
                        warnings.push_back("thickness override conflicts with IPC layer " +
                                           layer.name);
                    layer.thickness_m = match->second;
                    source = "override";
                }
            }
            if (!std::isfinite(layer.thickness_m) || layer.thickness_m <= 0)
                missing_thickness.push_back(layer.name);
            if (source == "override")
                source += ":" + options.stack_override.filename().string();
            rows.push_back({layer.name, layer.material, "", source});
            if (odb && records[i].odb_value_m)
                warnings.push_back("thickness override conflicts with ODB layer " + layer.name);
        }
        if (!missing_thickness.empty()) {
            std::string message = "IMPORT_REJECTED: missing thickness for layers:";
            for (const auto& name : missing_thickness)
                message += " " + name;
            return reject(message);
        }
        for (const auto& [name, thickness] : overrides.thicknesses) {
            (void)thickness;
            if (std::none_of(model.layers.begin(), model.layers.end(), [&](const auto& layer) {
                    return fold_layer_name(name) == fold_layer_name(layer.name);
                }))
                warnings.push_back("stack override layer absent from board: " + name);
        }
        // Overrides are stack material IDs: on copper they select the base,
        // while the source material still selects the metal. Keep every layer in
        // its original position so overridden dielectrics remain base candidates.
        auto map_file = options.material_map;
        StagedOutputs temporary_map;
        std::vector<bool> material_overridden(model.layers.size(), false);
        if (!overrides.materials.empty()) {
            // Validate the original object before appending overrides, preserving
            // malformed-map and ordinary duplicate-key diagnostics.
            ecad::EcadModel empty;
            std::string error;
            if (!ecad::apply_material_map(empty, map_file, &error))
                return reject(error, error.starts_with("MATERIAL_MAPPING_MISSING"));
            if (!ecad::validate_mapped_targets(empty, &error))
                return reject(error);
            std::ifstream input(map_file);
            std::string text((std::istreambuf_iterator<char>(input)), {});
            const auto end = text.find_last_of('}');
            if (!input || end == std::string::npos)
                return reject("MATERIAL_MAPPING_MISSING: cannot read material map", true);
            bool has_entries = text.find_first_not_of(" \t\r\n", text.find('{') + 1) != end;
            text.resize(end);
            for (std::size_t i = 0; i < model.layers.size(); ++i) {
                auto& layer = model.layers[i];
                const auto found = std::find_if(
                    overrides.materials.begin(), overrides.materials.end(), [&](const auto& entry) {
                        return fold_layer_name(entry.first) == fold_layer_name(layer.name);
                    });
                if (found == overrides.materials.end())
                    continue;
                material_overridden[i] = true;
                std::string key = "copper_base@" + layer.name;
                if (!layer.copper) {
                    // A private exact source key bypasses even an existing exact
                    // source-material mapping, without changing public interfaces.
                    key = "__chipcae_override_" + std::to_string(i);
                    while (
                        text.find(json_string(key)) != std::string::npos ||
                        std::any_of(model.layers.begin(), model.layers.end(), [&](const auto& l) {
                            return fold_layer_name(l.material) == fold_layer_name(key);
                        }))
                        key += '_';
                    layer.material = key;
                }
                if (has_entries)
                    text += ',';
                text += json_string(key) + ':' + json_string(found->second);
                has_entries = true;
            }
            text += '}';
            map_file = temporary_map.add(std::filesystem::temp_directory_path() /
                                         "chipcae-override-map.json");
            std::ofstream output(map_file);
            output << text;
            output.close();
            if (!output)
                return reject("cannot stage material overrides");
        }
        std::string error;
        if (!ecad::apply_material_map(model, map_file, &error))
            return reject(error, error.find("MATERIAL_MAPPING_MISSING") != std::string::npos);
        if (!ecad::validate_mapped_targets(model, &error))
            return reject(error);
        for (std::size_t i = 0; i < rows.size(); ++i)
            rows[i].material_id = rows[i].material_key.empty() && !material_overridden[i]
                                      ? ""
                                      : (model.layers[i].copper && !material_overridden[i]
                                             ? model.layers[i].metal
                                             : model.layers[i].material);

        std::vector<SkippedRow> skipped;
        auto defects = classify_unsupported(model.report, skipped, odb);
        std::vector<std::string> accepted;
        if (!options.accept_file.empty()) {
            const auto list =
                chipcae::geometry::read_accept_file(options.accept_file, &error, &warnings);
            if (!list)
                return reject(error.empty() ? "IMPORT_REJECTED: accept file does not exist"
                                            : error);
            accepted = chipcae::geometry::accepted_ids(*list);
        }
        auto policy = chipcae::geometry::apply_repair_policy(
            std::move(defects), accepted, options.accept_b_tier);
        warnings.insert(warnings.end(), policy.warnings.begin(), policy.warnings.end());
        // The frozen policy can overwrite Rejected with ConfirmationRequired.
        // Inspect the actual defects to make C rejection independent of ordering.
        const bool has_c = std::any_of(policy.defects.begin(),
                                       policy.defects.end(),
                                       [](const auto& d) { return d.tier == Tier::C; });
        chipcae::geometry::ImportReportHeader header;
        header.b_tier_accepted_all = options.accept_b_tier;
        if (!chipcae::geometry::write_import_report(
                import_report_directory(options.output), header, policy.defects, &error))
            return reject("IMPORT_REJECTED: " + error);
        for (const auto& notice : mask_notices)
            std::cerr << notice << '\n';
        for (const auto& warning : warnings)
            std::cerr << warning << '\n';
        if (has_c)
            return reject("IMPORT_REJECTED: unsupported board geometry");
        if (policy.result == chipcae::geometry::ImportResult::ConfirmationRequired) {
            write_defects(
                report, "IMPORT_CONFIRMATION_REQUIRED", "tier B defects require confirmation");
            std::cerr << "IMPORT_CONFIRMATION_REQUIRED: tier B defects require confirmation\n";
            return 6;
        }
        StagedOutputs outputs;
        const auto polyset = outputs.add(options.polyset);
        const auto stack = outputs.add(options.output);
        const auto layers =
            outputs.add(import_report_directory(options.output) / "import_layers.json");
        if (!ecad::to_stack(model, stack, polyset, &error))
            return reject("IMPORT_REJECTED: " + error);
        if (!write_layers(
                layers, model, rows, skipped, odb ? &records : nullptr, overrides.source, warnings))
            return reject("IMPORT_REJECTED: cannot write layer report");
        outputs.publish();
        write_defects(report, "OK", "");
        return CliExit::success;
    } catch (const std::exception& error) {
        return reject("IMPORT_REJECTED: " + std::string(error.what()));
    }
#endif
}

std::string ecad_help() {
    return "ODB++ / IPC-2581: chipcae-import <board.xml|board.tgz|board.zip|odb-directory> "
           "-o out.stack.json --material-map map.json [--stack-override override.json] "
           "[--report defects.json] [--accept import.accept.json | --accept-b-tier]\n";
}
} // namespace chipcae::importer
