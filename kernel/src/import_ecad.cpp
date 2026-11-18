#include "chipcae/geometry/import_accept.hpp"
#include "chipcae/geometry/import_report.hpp"
#include "import_common.hpp"
#include "stack_override.hpp"
#ifdef CHIPCAE_ENABLE_ECAD
#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "chipcae/geometry/ecad/odbpp.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <cmath>
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
// Percent encoding is injective, stable across source relocation and independent
// of standard-library hashes. Line numbers distinguish repeated source records.
std::string id_component(const std::string& value) {
    std::string out;
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
bool odb_input(const std::filesystem::path& input) {
    const auto name = fold_layer_name(input.filename().string());
    if (name.ends_with(".tgz") || name.ends_with(".tar.gz") || name.ends_with(".zip"))
        return true;
    std::error_code error;
    return std::filesystem::is_directory(input, error) &&
           std::filesystem::is_regular_file(input / "matrix/matrix", error);
}
struct SkippedRow {
    std::string name, function;
};
std::vector<Defect> classify_unsupported(const ecad::ImportReport& report,
                                         std::vector<SkippedRow>& skipped) {
    std::vector<Defect> defects;
    std::map<std::string, unsigned> occurrences;
    for (const auto& entry : report.unsupported) {
        const auto& text = entry.element;
        if (text.starts_with("SkippedLayer:")) {
            const auto end = text.rfind(':');
            if (end > 13 && end != std::string::npos)
                skipped.push_back({text.substr(13, end - 13), text.substr(end + 1)});
            continue;
        }
        Defect defect{};
        defect.tier = Tier::C;
        defect.kind = "IMPORT_UNSUPPORTED_ELEMENT";
        defect.dimension = "element";
        defect.size = 1;
        defect.suggestion = "unsupported geometry must be modeled before import";
        defect.defect_id = "board/" + id_component(text) + "/" + std::to_string(entry.line);
        const auto count = occurrences[defect.defect_id]++;
        if (count)
            defect.defect_id += "/" + std::to_string(count);
        const auto colon = text.find(':');
        const auto tag = text.substr(0, colon);
        // See docs/geometry/odbpp-import.md. Unknown geometry stays C; bare
        // XML extension names lack geometry context and require B confirmation.
        static const std::set<std::string> metadata{"SolderMask",
                                                    "LayerFunction",
                                                    "Component",
                                                    "Package",
                                                    "PackageRef",
                                                    "Text",
                                                    "Attribute",
                                                    "Property",
                                                    "Material",
                                                    "Spec",
                                                    "EntryStandard",
                                                    "Circle",
                                                    "StandardPrimitiveRef",
                                                    "selected step"};
        static const std::set<std::string> geometry{"Arc",
                                                    "Contour",
                                                    "Copper",
                                                    "Cutout",
                                                    "Line",
                                                    "Outline",
                                                    "Oval",
                                                    "Pad",
                                                    "Polygon",
                                                    "PolyStepCurve",
                                                    "Profile",
                                                    "ProfileHole",
                                                    "RectCenter",
                                                    "RectCham",
                                                    "RectCorner",
                                                    "RectRound",
                                                    "Slot",
                                                    "Symbol",
                                                    "Trace",
                                                    "UserPrimitiveRef",
                                                    "UserSpecial",
                                                    "Via",
                                                    "ViaGeometry",
                                                    "ViaSpan",
                                                    "ViaToolAmbiguous",
                                                    "ViaToolUnsupported"};
        const bool text_record = text.find(": unsupported record T") != std::string::npos;
        const bool metadata_path = text.find("/components") != std::string::npos ||
                                   text.starts_with("misc/") || text.starts_with("selected step ");
        if (!geometry.contains(tag) && (metadata.contains(tag) || colon == std::string::npos ||
                                        text_record || metadata_path)) {
            defect.tier = Tier::B;
            defect.suggestion = "confirm omitted metadata, components or text";
        }
        if (text.starts_with("SolderMask:"))
            defect.suggestion = "solder mask not modeled";
        if (text.starts_with("ViaPlatingMissing:")) {
            defect.tier = Tier::B;
            defect.suggestion = "confirm missing via plating";
            const auto start = std::string("ViaPlatingMissing:").size();
            const auto end = text.find(':', start);
            const auto number = end == std::string::npos ? end : text.find(':', end + 1);
            if (end != std::string::npos && number != std::string::npos) {
                std::uint64_t size = 0;
                const auto digits = std::string_view(text).substr(number + 1);
                const auto [last, error] =
                    std::from_chars(digits.data(), digits.data() + digits.size(), size);
                if (error == std::errc{} && last == digits.data() + digits.size()) {
                    defect.size = static_cast<double>(size);
                    defect.defect_id = "ecad/via_plating_missing/" +
                                       text.substr(start, end - start) + "-" +
                                       text.substr(end + 1, number - end - 1);
                }
            }
            // ODB's shorter diagnostic identifies the drill layer, with no
            // span/count available; keep its stable board ID and size=1.
        }
        defects.push_back(std::move(defect));
    }
    std::sort(skipped.begin(), skipped.end(), [](const auto& a, const auto& b) {
        return std::tie(a.name, a.function) < std::tie(b.name, b.function);
    });
    skipped.erase(std::unique(skipped.begin(),
                              skipped.end(),
                              [](const auto& a, const auto& b) {
                                  return a.name == b.name && a.function == b.function;
                              }),
                  skipped.end());
    return defects;
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
        write_defects(report, missing ? "MATERIAL_MAPPING_MISSING" : "IMPORT_REJECTED", message);
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
        std::vector<LayerSourceRow> rows;
        std::vector<std::string> warnings;
        std::vector<std::string> missing_thickness;
        for (std::size_t i = 0; i < model.layers.size(); ++i) {
            auto& layer = model.layers[i];
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
                if (!std::isfinite(layer.thickness_m) || layer.thickness_m <= 0)
                    missing_thickness.push_back(layer.name);
            }
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
        // Explicit material IDs bypass the map entirely. Map only the remaining
        // layers with the shared mapper, then merge IDs in the original order.
        ecad::EcadModel remaining;
        std::vector<std::size_t> indices;
        for (std::size_t i = 0; i < model.layers.size(); ++i) {
            const auto found = std::find_if(
                overrides.materials.begin(), overrides.materials.end(), [&](const auto& entry) {
                    return fold_layer_name(entry.first) == fold_layer_name(model.layers[i].name);
                });
            if (found != overrides.materials.end())
                model.layers[i].material = found->second;
            else {
                indices.push_back(i);
                remaining.layers.push_back(model.layers[i]);
            }
        }
        std::string error;
        if (!ecad::apply_material_map(remaining, options.material_map, &error))
            return reject(error, error.find("MATERIAL_MAPPING_MISSING") != std::string::npos);
        for (std::size_t i = 0; i < indices.size(); ++i)
            model.layers[indices[i]].material = remaining.layers[i].material;
        for (std::size_t i = 0; i < rows.size(); ++i)
            rows[i].material_id = rows[i].material_key.empty() ? "" : model.layers[i].material;

        std::vector<SkippedRow> skipped;
        auto defects = classify_unsupported(model.report, skipped);
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
