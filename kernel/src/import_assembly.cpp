#include "chipcae/geometry/assembly.hpp"
#include "chipcae/geometry/import_report.hpp"
#include "chipcae/geometry/step_import.hpp"
#include "import_common.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#if defined(CHIPCAE_ENABLE_JSONCONS)
#include <jsoncons/json.hpp>
#endif
namespace chipcae::importer {
namespace {
using namespace geometry;
inline std::string defect_json_quote(const std::string& s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\')
            out << '\\' << c;
        else if (c < 0x20)
            out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else
            out << c;
    }
    out << '"';
    return out.str();
}

inline void write_defect(std::ostream& out, const geometry::Defect& d) {
    const auto id = geometry::defect_id(d), tier = geometry::tier_name(d.tier);
    out << "{\"defect_id\":" << defect_json_quote(id) << ",\"tier\":" << defect_json_quote(tier)
        << ",\"kind\":" << defect_json_quote(d.kind)
        << ",\"reason\":" << static_cast<unsigned>(d.reason)
        << ",\"dimension\":" << defect_json_quote(d.dimension) << ",\"size\":";
    if (std::isfinite(d.size))
        out << std::setprecision(17) << d.size;
    else
        out << "null";
    out << ",\"size_unit\":" << defect_json_quote(geometry::defect_size_unit(d))
        << ",\"suggestion\":" << defect_json_quote(d.suggestion)
        << ",\"action\":" << defect_json_quote(d.action)
        << ",\"rejection_reason\":" << defect_json_quote(d.rejection_reason)
        << ",\"material_ambiguous\":" << (d.material_ambiguous ? "true" : "false");
    out << '}';
}
inline void write_defects_json(std::ostream& out,
                               const std::string& code,
                               const std::string& message,
                               const std::vector<geometry::Defect>& defects) {
    out << "{\"schema_version\":\"import-report/1.0\",\"code\":" << defect_json_quote(code)
        << ",\"message\":" << defect_json_quote(message) << ",\"defects\":[";
    bool first = true;
    for (const auto& d : defects) {
        if (!first)
            out << ',';
        first = false;
        write_defect(out, d);
    }
    out << "]}\n";
}

inline void write_assembly_defects(const std::filesystem::path& file,
                                   const std::string& code,
                                   const std::string& message,
                                   const std::vector<geometry::Defect>& defects) {
    if (file.empty())
        return;
    std::ofstream out(file);
    write_defects_json(out, code, message, defects);
    out.close();
    if (!out)
        throw std::runtime_error("cannot write defects output");
}

#if defined(CHIPCAE_ENABLE_JSONCONS)
inline std::vector<std::string> read_acceptance_list(const ImportOptions& options,
                                                     const std::filesystem::path& directory) {
    auto file = options.accept_file;
    if (file.empty() && !options.accept_b_tier) {
        auto candidate = directory / "import.accept.json";
        if (std::filesystem::exists(candidate))
            file = candidate;
    }
    std::vector<std::string> accepted;
    if (!file.empty()) {
        std::ifstream input(file);
        auto document = jsoncons::json::parse(input);
        for (const auto& item : document["accept"].array_range())
            accepted.push_back(item["defect_id"].as<std::string>());
    }
    return accepted;
}

#endif
#if defined(CHIPCAE_ENABLE_JSONCONS)
// Read the protocol's list of scalar error records (flow or block style).
// Only code/name are consumed; comments and quoted scalars are accepted.
inline std::optional<int> protocol_error_code(const std::string& name, std::istream& input) {
    const std::regex field(R"re((code|name)\s*:\s*["']?([A-Za-z_0-9]+)["']?)re");
    std::string line, record;
    auto lookup = [&]() -> std::optional<int> {
        std::string entry_name, code;
        for (auto it = std::sregex_iterator(record.begin(), record.end(), field);
             it != std::sregex_iterator();
             ++it) {
            if ((*it)[1] == "name")
                entry_name = (*it)[2];
            else
                code = (*it)[2];
        }
        if (entry_name == name && !code.empty()) {
            try {
                std::size_t used = 0;
                const int value = std::stoi(code, &used);
                if (used == code.size())
                    return value;
            } catch (const std::exception&) {
            }
        }
        return std::nullopt;
    };
    while (std::getline(input, line)) {
        // Protocol scalar values do not contain '#'.
        line.erase(line.find('#') == std::string::npos ? line.size() : line.find('#'));
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line[first] == '-') {
            if (auto found = lookup())
                return found;
            record.clear();
        }
        record += line + '\n';
    }
    return lookup();
}
#endif
inline std::string assembly_error_name(const std::string& message) {
    const auto name = message.substr(0, message.find(':'));
    return name == "outline.unsupported" ? "CUT_UNSUPPORTED" : name;
}
inline std::string import_failure_message(const std::string& code, const std::string& message) {
    return std::regex_search(message, std::regex(R"(^([A-Z_]+|outline\.unsupported): )"))
               ? message
               : code + ": " + message;
}
#if defined(CHIPCAE_ENABLE_JSONCONS)
inline int protocol_error_exit(const std::string& message, std::istream& in) {
    return protocol_error_code(assembly_error_name(message), in).value_or(1);
}
#endif
// Private assembly report extension.
struct ImportReportParts {
    std::map<std::string, std::vector<std::string>> split_layers;
    std::vector<std::pair<std::string, std::vector<std::string>>> rewrite_targets;
    std::vector<std::string> material_approx;
};

#if defined(CHIPCAE_ENABLE_JSONCONS)
inline bool write_assembly_report(const std::filesystem::path& dir,
                                  const ImportReportHeader& h,
                                  const std::vector<Defect>& ds,
                                  std::string* error,
                                  const ImportReportParts* parts) {
    if (!geometry::write_import_report(dir, h, ds, error))
        return false;
    try {
        using J = jsoncons::json;
        std::ifstream input(dir / "import_report.json");
        auto report = J::parse(input);
        input.close();
        std::map<std::string, std::string> details;
        for (const auto& d : ds)
            details.emplace(geometry::defect_id(d), d.details);
        for (auto& entry : report["defects"].array_range()) {
            const auto found = details.find(entry["defect_id"].as<std::string>());
            if (found != details.end())
                entry["details"] = found->second;
        }
        if (parts) {
            auto& summary = report["parts"];
            summary["split_layers"] = J::object();
            for (const auto& [name, targets] : parts->split_layers)
                summary["split_layers"][name] = targets;
            summary["rewrite_targets"] = J::array();
            for (const auto& [from, targets] : parts->rewrite_targets) {
                J entry;
                entry["from"] = from;
                entry["to"] = targets;
                summary["rewrite_targets"].push_back(entry);
            }
            summary["material_approx"] = J::array();
            for (const auto& id : parts->material_approx) {
                J entry;
                entry["id"] = id;
                entry["material_approx"] = true;
                entry["label"] = "近似材料";
                summary["material_approx"].push_back(entry);
            }
        }
        std::ofstream output(dir / "import_report.json");
        output << report << '\n';
        output.close();
        if (!output)
            throw std::runtime_error("cannot write report output");
        return true;
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
        return false;
    }
}
#else
inline bool write_assembly_report(const std::filesystem::path& dir,
                                  const ImportReportHeader& h,
                                  const std::vector<Defect>& ds,
                                  std::string* error,
                                  const ImportReportParts*) {
    return geometry::write_import_report(dir, h, ds, error);
}
#endif

#if defined(CHIPCAE_ENABLE_JSONCONS)
std::filesystem::path assembly_root() {
    // Linux runtime packaging uses /proc/self/exe to locate assets independently of cwd.
    std::error_code ec;
    auto executable = std::filesystem::read_symlink("/proc/self/exe", ec);
    auto dir = ec ? std::filesystem::current_path() : executable.parent_path();
    const auto installed = dir / "../share/chipcae";
    if (std::filesystem::exists(installed / "protocol/errors.yaml"))
        return installed;
    for (auto candidate = dir; !candidate.empty();) {
        if (std::filesystem::exists(candidate / "import-assets/protocol/errors.yaml"))
            return candidate / "import-assets";
        auto parent = candidate.parent_path();
        if (parent == candidate)
            break;
        candidate = parent;
    }
    return installed;
}
#endif
const char* step_kind(geometry::StepDefectKind k) {
    using K = geometry::StepDefectKind;
#define KIND(x)                                                                                    \
    case K::x:                                                                                     \
        return #x
    switch (k) {
        KIND(Gap);
        KIND(OpenEdge);
        KIND(DuplicateFace);
        KIND(FlippedFace);
        KIND(NonManifoldEdge);
        KIND(SmallEdge);
        KIND(SmallFace);
        KIND(SliverFace);
        KIND(WireSelfIntersection);
        KIND(SolidOverlap);
        KIND(OpenShell);
        KIND(NearLayerInterface);
        KIND(UnitSuspect);
        KIND(FaceSelfIntersection);
        KIND(InvalidGeometry);
        KIND(ToleranceExceeded);
    }
#undef KIND
    return "InvalidGeometry";
}
} // namespace
std::vector<geometry::Defect>
map_step_defects(const std::string& part,
                 const std::vector<geometry::StepDefect>& input,
                 const std::optional<std::string>& body = std::nullopt) {
    std::vector<geometry::Defect> out;
    std::set<std::string> ids;
    for (const auto& s : input) {
        if (body && !s.body_name.empty() && s.body_name != *body)
            continue;
        geometry::Defect d{};
        d.tier = s.tier;
        d.reason = geometry::DefectReason::TOLERANCE_RANGE_UNSUPPORTED;
        d.kind = std::string("step.") + step_kind(s.kind);
        d.dimension = "measured";
        d.size = s.measured;
        d.defect_id = part + "/" + s.defect_id;
        if (ids.contains(d.defect_id) && !s.status.empty())
            d.defect_id += "#" + s.status;
        const auto candidate = d.defect_id;
        for (unsigned suffix = 2; ids.contains(d.defect_id); ++suffix)
            d.defect_id = candidate + "#" + std::to_string(suffix);
        ids.insert(d.defect_id);
        d.action = s.action;
        d.details = "step_entity=" + s.step_entity + ";body=" + s.body_name +
                    ";face=" + std::to_string(s.face_index) + ";status=" + s.status;
        out.push_back(d);
    }
    return out;
}
mesh::Expected<bool> inline_assembly_outline(mesh::StackModel& stack, const mesh::Polyset& poly) {
    if (!stack.outline.polygon.empty())
        return true;
    const decltype(poly.layers)::value_type* outline = nullptr;
    for (const auto& l : poly.layers)
        if (l.name == "outline") {
            if (outline || l.ring_offsets.size() != 2)
                return mesh::Error{"$/outline", "CUT_UNSUPPORTED: outline needs one ring"};
            outline = &l;
        }
    if (outline) {
        const auto& offsets = outline->ring_offsets;
        const auto& coordinates = outline->coordinates;
        if (coordinates.size() % 2 != 0 || offsets[0] != 0 || offsets[1] < 3 ||
            static_cast<std::uint64_t>(offsets[1]) != coordinates.size() / 2)
            return mesh::Error{"$/outline",
                               "CUT_UNSUPPORTED: invalid outline coordinates or offsets"};
        for (std::size_t i = 0; i < coordinates.size(); i += 2)
            stack.outline.polygon.push_back({static_cast<double>(coordinates[i]) * poly.dbu_m,
                                             static_cast<double>(coordinates[i + 1]) * poly.dbu_m});
    }
    if (!stack.outline.polygon.empty()) {
        const auto& first = stack.outline.polygon.front();
        stack.outline.box = {first[0], first[1], first[0], first[1]};
        for (const auto& p : stack.outline.polygon) {
            stack.outline.box[0] = std::min(stack.outline.box[0], p[0]);
            stack.outline.box[1] = std::min(stack.outline.box[1], p[1]);
            stack.outline.box[2] = std::max(stack.outline.box[2], p[0]);
            stack.outline.box[3] = std::max(stack.outline.box[3], p[1]);
        }
    }
    return true;
}
#if defined(CHIPCAE_ENABLE_JSONCONS)
inline mesh::Expected<bool>
inline_assembly_outline_references(mesh::StackModel& stack,
                                   const std::filesystem::path& board_path) {
    if (!stack.outline.polygon.empty())
        return true;
    std::ifstream input(board_path);
    const auto board = jsoncons::json::parse(input);
    if (!board.contains("external_refs"))
        return true;
    for (const auto& ref : board["external_refs"].array_range()) {
        auto poly = mesh::read_polyset(board_path.parent_path() / ref["path"].as<std::string>());
        if (!poly)
            return poly.error();
        auto outline = inline_assembly_outline(stack, poly.value());
        if (!outline)
            return outline.error();
        if (!stack.outline.polygon.empty())
            break;
    }
    return true;
}
#endif
std::vector<std::string> interface_rewrite_targets(const std::vector<std::string>& lower,
                                                   const std::vector<std::string>& upper) {
    std::vector<std::string> targets;
    for (const auto& a : lower)
        for (const auto& b : upper)
            targets.push_back("interface:" + a + "|" + b);
    return targets;
}
inline void summarize_assembly_parts(const mesh::StackModel& base,
                                     const mesh::StackModel& merged,
                                     ImportReportParts& parts) {
    auto rewrite = [&](const std::string& from, const std::vector<std::string>& targets) {
        if (targets.empty())
            return;
        parts.rewrite_targets.emplace_back(from, targets);
    };
    std::map<std::string, std::vector<std::string>> layer_targets;
    for (const auto& l : base.layers) {
        std::vector<std::string> split, targets;
        for (const auto& n : merged.layers)
            if (n.name == l.name || n.name.starts_with(l.name + "#")) {
                targets.push_back(n.name);
                if (n.name != l.name)
                    split.push_back(n.name);
            }
        layer_targets[l.name] = targets;
        if (!split.empty()) {
            parts.split_layers[l.name] = split;
            std::vector<std::string> qualified;
            for (const auto& target : targets)
                qualified.push_back("layer:" + target);
            rewrite("layer:" + l.name, qualified);
        }
        for (const auto& r : l.regions) {
            std::vector<std::string> rt;
            for (const auto& n : merged.layers)
                for (const auto& nr : n.regions)
                    if (nr.name == r.name || nr.name.starts_with(r.name + "#"))
                        rt.push_back("region:" + nr.name);
            if (rt != std::vector<std::string>{"region:" + r.name})
                rewrite("region:" + r.name, rt);
        }
    }
    for (std::size_t i = 1; i < base.layers.size(); ++i) {
        const auto& a = base.layers[i - 1].name;
        const auto& b = base.layers[i].name;
        if (layer_targets[a].empty() || layer_targets[b].empty())
            continue;
        auto targets = interface_rewrite_targets(layer_targets[a], layer_targets[b]);
        auto from = "interface:" + a + "|" + b;
        if (targets != std::vector<std::string>{from})
            rewrite(from, targets);
    }
}

namespace {
// Merge preserves base layer identity (including split pieces) and part body IDs.
// Use layer indices so bottom extensions and rounded serialized thicknesses cannot
// change which body endpoint touches an empty layer.
[[maybe_unused]] std::optional<std::string>
floating_gap_part(const mesh::StackModel& merged,
                  const mesh::StackModel& base,
                  const std::vector<geometry::PlacedPart>& placed) {
    const auto count = merged.layers.size();
    std::size_t board_first = count, board_last = 0;
    std::map<std::string, std::pair<std::size_t, std::size_t>> body_span;
    for (std::size_t i = 0; i < count; ++i) {
        const auto& layer = merged.layers[i];
        for (const auto& original : base.layers) {
            const auto& body = original.body.empty() ? original.name : original.body;
            if ((layer.name == original.name || layer.name.starts_with(original.name + "#")) &&
                (layer.body == body || layer.body == original.body)) {
                board_first = std::min(board_first, i);
                board_last = i;
                break;
            }
        }
        for (const auto& region : layer.regions) {
            auto [it, inserted] = body_span.try_emplace(region.body, i, i);
            if (!inserted)
                it->second.second = i;
        }
    }
    if (board_first == count)
        return std::nullopt;
    // Layers run bottom to top: report the lowest cavity first, then use placed order.
    for (std::size_t i = 0; i < count; ++i) {
        const auto& layer = merged.layers[i];
        if (!layer.material.empty() || !layer.regions.empty())
            continue;
        for (const auto& part : placed) {
            if (part.id.empty())
                continue;
            const auto span = body_span.find(part.id);
            if (span != body_span.end() && ((i > board_last && span->second.first == i + 1) ||
                                            (i < board_first && span->second.second + 1 == i)))
                return part.id;
        }
        // Missing body IDs may fall back only to an outer slab endpoint on the
        // far side of this cavity. Base thicknesses and placed slabs use the
        // board's original frame. Translate both into the merged stack frame
        // using the layers below the board, including any bottom extension.
        const bool above = i > board_last;
        if (!above && i >= board_first)
            return std::nullopt;
        Real board_bottom = 0;
        for (std::size_t j = 0; j < board_first; ++j)
            board_bottom += merged.layers[j].thickness;
        Real boundary = board_bottom;
        if (above) {
            for (const auto& original : base.layers)
                boundary += original.thickness;
            for (std::size_t j = board_last + 1; j <= i; ++j)
                boundary += merged.layers[j].thickness;
        } else {
            for (std::size_t j = board_first; j > i; --j)
                boundary -= merged.layers[j - 1].thickness;
        }
        for (const auto& part : placed) {
            if (body_span.contains(part.id) || part.slabs.empty() || part.id.empty())
                continue;
            Real endpoint = above ? part.slabs.front().z0 : part.slabs.front().z1;
            for (const auto& slab : part.slabs)
                endpoint = above ? std::min(endpoint, slab.z0) : std::max(endpoint, slab.z1);
            endpoint += board_bottom;
            // Independent thickness sums may round a shared endpoint differently.
            // Use the default absolute (metres) and relative numeric tolerances.
            constexpr Real abs_tol = 1e-10, rel_tol = 5.5e-6;
            if (std::abs(endpoint - boundary) <=
                abs_tol + rel_tol * std::max(std::abs(endpoint), std::abs(boundary)))
                return part.id;
        }
        // Keep schema rejection if the first cavity cannot be attributed safely.
        return std::nullopt;
    }
    return std::nullopt;
}
} // namespace

bool assembly_display_only(const geometry::StepImportResult& result) {
    return std::any_of(result.body_names.begin(), result.body_names.end(), [](const auto& name) {
        return name.find("NOT_CAE_GEOMETRY") != std::string::npos;
    });
}
std::optional<geometry::PlacedPart>
assembly_placement_result(geometry::Expected<geometry::PlacedPart> result,
                          const std::string& id,
                          const std::string& material,
                          std::vector<geometry::Defect>& defects) {
    if (!result) {
        geometry::Defect d{};
        d.tier = geometry::Tier::C;
        d.reason = geometry::DefectReason::PART_ATTACH_INVALID;
        d.defect_id = id + "/attach_invalid";
        d.details = result.error().message;
        defects.push_back(d);
        return std::nullopt;
    }
    auto part = result.value();
    part.id = id;
    part.material = material;
    return part;
}
#if defined(CHIPCAE_ENABLE_JSONCONS)
int assembly_error_exit(const std::string& message) {
    const auto name = assembly_error_name(message);
    if (name == "IMPORT_CONFIRMATION_REQUIRED")
        return CliExit::import_confirmation_required;
    if (name == "IMPORT_REJECTED")
        return CliExit::import_rejected;
    std::ifstream in(assembly_root() / "protocol/errors.yaml");
    return protocol_error_exit(message, in);
}
#endif
} // namespace chipcae::importer
#ifdef CHIPCAE_ASSEMBLY_CLI
#include "stack_writer.hpp"
#if defined(CHIPCAE_ENABLE_JSONCONS)
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#ifdef CHIPCAE_ENABLE_ECAD
#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "import_ecad_defects.hpp"
#ifdef CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE
#include "chipcae/geometry/ecad/odbpp.hpp"
#endif
#endif
namespace chipcae::importer {
int run_assembly(const ImportOptions& options) {
    using namespace geometry;
    using J = jsoncons::json;
    std::vector<Defect> defects;
    ImportReportHeader header{};
    header.welded_interfaces = 0;
    ImportReportParts parts;
    auto reports = [&](const std::string& code, const std::string& message) {
        deduplicate_defect_ids(defects);
        std::string error;
        const auto dir = import_report_directory(options.output);
        if (!write_assembly_report(dir, header, defects, &error, &parts))
            throw std::runtime_error(error);
        write_assembly_defects(options.report, code, message, defects);
    };
    auto fail = [&](const std::string& code, const std::string& message) {
        const auto full = import_failure_message(code, message);
        const auto exit = assembly_error_exit(full);
        const auto prefix = exit == 1 ? "INTERNAL" : assembly_error_name(full);
        reports(prefix, full);
        std::cerr << full << '\n';
        return exit;
    };
    try {
        std::filesystem::create_directories(import_report_directory(options.output));
        if (std::filesystem::weakly_canonical(
                options.polyset.parent_path().empty() ? "." : options.polyset.parent_path()) !=
            std::filesystem::weakly_canonical(
                options.output.parent_path().empty() ? "." : options.output.parent_path()))
            return fail("INPUT_INVALID", "INPUT_INVALID: polyset must be in output directory");
        J doc;
        const auto assembly_location = "assembly=" + options.assembly.string() + "; json_path=$: ";
        std::ifstream assembly_input(options.assembly);
        if (!assembly_input)
            return fail("IMPORT_REJECTED", assembly_location + "cannot open assembly");
        try {
            doc = J::parse(assembly_input);
        } catch (const std::exception& e) {
            return fail("INPUT_INVALID", assembly_location + e.what());
        }
        std::string invalid;
        try {
            for (const auto& asset : {"schema/assembly-0.1.schema.json",
                                      "schema/stack-0.2.schema.json",
                                      "protocol/errors.yaml"}) {
                std::ifstream file(assembly_root() / asset);
                if (!file)
                    throw std::runtime_error(std::string("cannot open runtime asset ") + asset);
                if (std::string(asset).ends_with(".json"))
                    (void)jsoncons::jsonschema::make_json_schema(J::parse(file));
                else if (protocol_error_code("INTERNAL", file) != std::optional<int>(1))
                    throw std::runtime_error("invalid runtime asset protocol/errors.yaml");
            }
            std::ifstream spec(assembly_root() / "schema/assembly-0.1.schema.json");
            auto validator = jsoncons::jsonschema::make_json_schema(J::parse(spec));
            validator.validate(doc, [&](const jsoncons::jsonschema::validation_message& m) {
                if (invalid.empty())
                    invalid = "json_path=$" + m.instance_location().string() + ": " + m.message();
                return jsoncons::jsonschema::walk_result::advance;
            });
        } catch (const std::exception& e) {
            // The protocol itself may be missing or invalid. Use the fixed
            // bootstrap rejection rather than trying to read that asset again.
            const auto message = std::string("IMPORT_REJECTED: runtime assets: ") + e.what();
            reports("IMPORT_REJECTED", message);
            std::cerr << message << '\n';
            return CliExit::import_rejected;
        }
        if (!invalid.empty())
            return fail("INPUT_INVALID", invalid);
        const auto root = options.assembly.parent_path();
        const auto board_format = doc["board"]["format"].as<std::string>();
        const auto board_path = std::filesystem::path(doc["board"]["path"].as<std::string>());
        if (board_format != "ipc2581" && board_format != "odbpp")
            return fail("INPUT_INVALID", "json_path=$/board/format: unsupported board format");
        const auto resolved_board = std::filesystem::weakly_canonical(root / board_path);
        // The assembly schema applies the same lexical path rule to board,
        // material-map and STEP inputs. Do not add board-only symlink confinement.
        if (board_path.empty() ||
            (board_format == "ipc2581" ? !std::filesystem::is_regular_file(resolved_board)
                                       : !std::filesystem::exists(resolved_board)))
            return fail("INPUT_INVALID", "json_path=$/board/path: invalid or missing board path");
        std::set<std::string> ids;
        for (const auto& p : doc["parts"].array_range()) {
            auto id = p["id"].as<std::string>();
            if (id == "void")
                return fail("INPUT_INVALID", "INPUT_INVALID: part id void is reserved");
            if (!ids.insert(id).second)
                return fail("IMPORT_REJECTED", "duplicate part id " + id);
            if (p["step"].as<std::string>().find("NOT_CAE_GEOMETRY") != std::string::npos)
                return fail("IMPORT_REJECTED",
                            "IMPORT_REJECTED: display-only geometry NOT_CAE_GEOMETRY");
        }
        std::vector<std::string> accepted;
        try {
            accepted = read_acceptance_list(options, root);
        } catch (const std::exception& e) {
            return fail("INPUT_INVALID", std::string("accept: ") + e.what());
        }
#ifndef CHIPCAE_ENABLE_ECAD
        return fail("IMPORT_REJECTED", "ECAD support disabled");
#else
        const auto& b = doc["board"];
        auto read_board = [&]() -> ecad::ImportResult {
            if (b["format"].as<std::string>() == "odbpp") {
#ifdef CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE
                return ecad::read_odbpp(root / b["path"].as<std::string>());
#else
                ecad::ImportResult unavailable;
                unavailable.error = "IMPORT_REJECTED: odbpp reader not yet available";
                return unavailable;
#endif
            }
            return ecad::read_ipc2581(root / b["path"].as<std::string>());
        };
        auto board = read_board();
        if (!board)
            return fail("IMPORT_REJECTED", board.error);
        std::string error;
        if (!ecad::apply_material_map(
                board.model, root / b["material_map"].as<std::string>(), &error))
            return fail("MATERIAL_MAPPING_MISSING", error);
        {
            auto bd = ecad_defects::board_defects(board.model.report,
                                                  b["format"].as<std::string>() == "odbpp");
            defects.insert(defects.end(), bd.begin(), bd.end());
        }
        // Intermediate board JSON is removed before confirmation; the sidecar is retained.
        auto temp = options.output;
        temp += ".board.tmp";
        struct Cleanup {
            std::filesystem::path p;
            ~Cleanup() {
                std::error_code ec;
                std::filesystem::remove(p, ec);
            }
        } cleanup{temp};
        if (!ecad::to_stack(board.model, temp, options.polyset, &error))
            return fail("IMPORT_REJECTED", error);
        auto loaded = mesh::load_stack(temp, assembly_root() / "schema/stack-0.2.schema.json");
        if (!loaded)
            return fail("INPUT_INVALID", loaded.error().reason);
        auto base = loaded.value();
        auto outline = inline_assembly_outline_references(base, temp);
        if (!outline)
            return fail(outline.error().reason.starts_with("CUT_UNSUPPORTED:") ? "CUT_UNSUPPORTED"
                                                                               : "INPUT_INVALID",
                        outline.error().reason);
        ImportScales scales;
        scales.L = std::max(base.outline.box[2] - base.outline.box[0],
                            base.outline.box[3] - base.outline.box[1]);
        if (base.layers.empty())
            return fail("INPUT_INVALID", "board has no layers");
        scales.t_min = base.layers.front().thickness;
        for (const auto& l : base.layers)
            scales.t_min = std::min(scales.t_min, l.thickness);
        scales.h_xy = scales.L / 100;
        scales.dbu_m = board.model.dbu_m;
        auto tol = compute_tolerances(scales, {});
        if (!tol)
            return fail("IMPORT_REJECTED", tol.error().message);
        header.L = scales.L;
        header.t_min = scales.t_min;
        header.tolerances = tol.value();
        header.b_tier_accepted_all = options.accept_b_tier;
        auto read_step = [&](const J& p) {
            StepImportOptions io;
            io.tolerances = tol.value();
            try {
                auto result = import_step(root / p["step"].as<std::string>(), io);
                if (assembly_display_only(result))
                    throw std::runtime_error("display-only geometry");
                return result;
            } catch (const std::exception& e) {
                throw std::runtime_error(import_failure_message("IMPORT_REJECTED", e.what()));
            }
        };
        std::vector<J> all_parts;
        std::map<std::string, StepImportResult> shared_steps;
        for (const auto& p : doc["parts"].array_range())
            all_parts.push_back(p);
        if (doc.contains("parts_from_step")) {
            const auto& f = doc["parts_from_step"];
            for (const auto& kv : f["bodies"].object_range()) {
                auto id = kv.value()["id"].as<std::string>();
                if (id == "void")
                    return fail("INPUT_INVALID", "INPUT_INVALID: part id void is reserved");
                if (!ids.insert(id).second)
                    return fail("IMPORT_REJECTED", "duplicate part id " + id);
            }
            if (f["step"].as<std::string>().find("NOT_CAE_GEOMETRY") != std::string::npos)
                return fail("IMPORT_REJECTED", "IMPORT_REJECTED: display-only geometry");
            std::optional<StepImportResult> imported;
            std::vector<std::string> names;
            if (step_import_available()) {
                imported = read_step(f);
                std::set<std::string> unique;
                for (const auto& name : imported->body_names) {
                    if (!unique.insert(name).second)
                        return fail("IMPORT_REJECTED", "duplicate body name " + name);
                    if (!f["bodies"].contains(name)) {
                        Defect d{};
                        d.tier = Tier::B;
                        d.reason = DefectReason::PART_UNMAPPED;
                        d.defect_id = "unmapped/" + name;
                        defects.push_back(d);
                    } else
                        names.push_back(name);
                }
#ifndef CHIPCAE_ASSEMBLY_GEOMETRY_AVAILABLE
                if (!names.empty())
                    return fail("IMPORT_REJECTED", "parts_from_step geometry unavailable");
#endif
            } else
                for (const auto& kv : f["bodies"].object_range())
                    names.emplace_back(kv.key());
            for (const auto& name : names) {
                J p = f["bodies"][name];
                p["step"] = f["step"];
                p["body"] = name;
                p["placement"]["frame"] = "step";
                p["placement"]["origin"] = f["origin"];
                if (imported)
                    shared_steps.emplace(p["id"].as<std::string>(), *imported);
                all_parts.push_back(p);
            }
        }
        auto attachment_defect = [&](const std::string& id, const std::string& message) {
            Defect d{};
            d.tier = Tier::C;
            d.reason = DefectReason::PART_ATTACH_INVALID;
            d.defect_id = id + "/attach_invalid";
            d.details = message;
            defects.push_back(d);
        };
        auto pending = all_parts;
        std::vector<J> ordered;
        std::set<std::string> done;
        while (!pending.empty()) {
            bool progress = false;
            for (auto it = pending.begin(); it != pending.end();) {
                const auto id = (*it)["id"].as<std::string>();
                const auto& v = (*it)["placement"];
                if (v.contains("attach")) {
                    auto ref = v["attach"].as<std::string>();
                    auto target = ref.substr(0, ref.rfind('@'));
                    if (target.starts_with("layer:")) {
                        auto name = target.substr(6);
                        if (std::none_of(base.layers.begin(),
                                         base.layers.end(),
                                         [&](const mesh::Layer& l) { return l.name == name; })) {
                            attachment_defect(id, "missing attach target " + target);
                            return fail("IMPORT_REJECTED",
                                        "IMPORT_REJECTED: missing attach target " + target);
                        }
                    } else if (!done.contains(target.substr(5))) {
                        if (std::none_of(pending.begin(), pending.end(), [&](const J& part) {
                                return part["id"].as<std::string>() == target.substr(5);
                            })) {
                            attachment_defect(id, "missing attach target " + target);
                            return fail("IMPORT_REJECTED",
                                        "IMPORT_REJECTED: missing attach target " + target);
                        }
                        ++it;
                        continue;
                    }
                }
                done.insert(id);
                ordered.push_back(*it);
                pending.erase(it);
                progress = true;
                // Reconsider earlier parts as soon as their dependency is ready.
                break;
            }
            if (!progress) {
                attachment_defect(pending.front()["id"].as<std::string>(),
                                  "cyclic part attachments");
                return fail("IMPORT_REJECTED", "IMPORT_REJECTED: cyclic part attachments");
            }
        }
        std::vector<PlacedPart> placed;
        auto process = [&](const J& p, const StepImportResult* shared = nullptr) {
            auto id = p["id"].as<std::string>();
            if (id == "void")
                throw std::runtime_error("INPUT_INVALID: part id void is reserved");
            for (const auto& l : base.layers) {
                for (const auto& field : {l.name, l.body})
                    if (id == field)
                        throw std::runtime_error("INPUT_INVALID: part id collides with " + field);
                for (const auto& r : l.regions)
                    for (const auto& field : {r.name, r.body})
                        if (id == field)
                            throw std::runtime_error("INPUT_INVALID: part id collides with " +
                                                     field);
            }
            if (p.get_value_or<bool>("material_approx", false)) {
                parts.material_approx.push_back(id);
            }
            if (!step_import_available()) {
                Defect d{};
                d.tier = Tier::C;
                d.reason = DefectReason::OCCT_UNAVAILABLE;
                d.defect_id = id + "/occt_unavailable";
                defects.push_back(d);
                return;
            }
            auto step = shared ? *shared : read_step(p);
            std::set<std::string> names;
            for (const auto& n : step.body_names) {
                if (!names.insert(n).second)
                    throw std::runtime_error("IMPORT_REJECTED: duplicate body name " + n);
            }
            if (!p.contains("body") && step.body_names.size() > 1)
                throw std::runtime_error("IMPORT_REJECTED: body is required for multi-body STEP");
            const auto body = p.get_value_or<std::string>(
                "body", step.body_names.empty() ? "" : step.body_names.front());
            if (!names.contains(body))
                throw std::runtime_error("IMPORT_REJECTED: unknown body name " + body);
            auto mapped = map_step_defects(id, step.defects, body);
            defects.insert(defects.end(), mapped.begin(), mapped.end());
            auto sliced = slice_prismatic(
                step, body, {tol.value().tau_A, tol.value().tau_A, tol.value().tau_B});
            for (auto d : sliced.defects) {
                d.defect_id = id + "/" + d.defect_id;
                defects.push_back(d);
            }
            if (sliced.slabs.empty()) {
                Defect d{};
                d.tier = Tier::C;
                d.reason = DefectReason::PART_NOT_PRISMATIC;
                d.defect_id = id + "/no_slabs";
                d.details = "slicing produced no slabs";
                defects.push_back(d);
                return;
            }
            Placement placement;
            const auto& v = p["placement"];
            if (v.contains("attach")) {
                AttachPlacement a;
                auto target = v["attach"].as<std::string>();
                auto at = target.rfind('@');
                a.target = target.substr(0, at);
                a.side = target.substr(at + 1) == "top" ? AttachSide::top : AttachSide::bottom;
                a.gap = v.get_value_or<double>("gap", 0);
                a.rot_z_deg = v.get_value_or<double>("rot_z", 0);
                if (v.contains("xy"))
                    a.xy = v["xy"].as<std::array<Real, 2>>();
                if (v.contains("bond"))
                    a.bond = Bond{v["bond"]["material"].as<std::string>(),
                                  v["bond"]["thickness"].as<Real>()};
                if (a.target.starts_with("part:")) {
                    const auto id_target = a.target.substr(5);
                    auto target =
                        std::find_if(placed.begin(), placed.end(), [&](const PlacedPart& pp) {
                            return pp.id == id_target;
                        });
                    if (target != placed.end() && !target->slabs.empty()) {
                        Real z = a.side == AttachSide::top ? target->slabs.front().z1
                                                           : target->slabs.front().z0;
                        for (const auto& slab : target->slabs)
                            z = a.side == AttachSide::top ? std::max(z, slab.z1)
                                                          : std::min(z, slab.z0);
                        a.resolved_z = z;
                    }
                } else if (a.bond && a.target.starts_with("layer:")) {
                    const auto layer_name = a.target.substr(6);
                    Real z = 0;
                    for (const auto& layer : base.layers) {
                        if (layer.name == layer_name) {
                            a.resolved_z = a.side == AttachSide::top ? z + layer.thickness : z;
                            break;
                        }
                        z += layer.thickness;
                    }
                }
                placement = a;
            } else
                placement = FramePlacement{v["origin"].as<std::array<Real, 3>>()};
            auto pp = assembly_placement_result(
                place(sliced.slabs, placement, base), id, p["material"].as<std::string>(), defects);
            if (pp) {
                placed.push_back(*pp);
                if (const auto* attach = std::get_if<AttachPlacement>(&placement);
                    attach && attach->bond) {
                    const auto& bond = *attach->bond;
                    Slab bond_slab;
                    if (attach->side == AttachSide::top) {
                        const auto& s = *std::min_element(
                            pp->slabs.begin(), pp->slabs.end(), [](const Slab& a, const Slab& b) {
                                return a.z0 < b.z0;
                            });
                        bond_slab = Slab{s.z0 - bond.thickness, s.z0, s.sections};
                    } else {
                        const auto& s = *std::max_element(
                            pp->slabs.begin(), pp->slabs.end(), [](const Slab& a, const Slab& b) {
                                return a.z1 < b.z1;
                            });
                        bond_slab = Slab{s.z1, s.z1 + bond.thickness, s.sections};
                    }
                    // Use the exact global face supplied to place() for both target kinds.
                    const Real face = attach->resolved_z.value();
                    // Undo placement roundoff only at the original attachment face.
                    Real& outer = attach->side == AttachSide::top ? bond_slab.z0 : bond_slab.z1;
                    // S includes |T|, |s.z| (the global face), and the largest source slab |z|.
                    // See handoff/rulings/pr242.md (architect sign-off, 2026-11-21 18:02)
                    // and handoff/rulings/i-assembly-bond-slab.md section 2 erratum.
                    Real scale = std::max(std::abs(bond.thickness), std::abs(face));
                    // Translation can cancel source z coordinates much larger than the bond.
                    for (const auto& slab : sliced.slabs)
                        scale = std::max({scale, std::abs(slab.z0), std::abs(slab.z1)});
                    const Real bound = 4 * std::numeric_limits<Real>::epsilon() * scale;
                    if (std::abs(outer - face) > bound)
                        throw std::runtime_error("INTERNAL: bond does not meet attachment face");
                    outer = face;
                    placed.push_back(PlacedPart{id + ".bond", bond.material, {bond_slab}});
                }
            }
        };
        for (const auto& p : ordered) {
            auto shared = shared_steps.find(p["id"].as<std::string>());
            process(p, shared == shared_steps.end() ? nullptr : &shared->second);
        }
        auto merged = base;
        if (!placed.empty()) {
            auto m =
                merge_parts(base, placed, {scales.t_min, tol.value().eps_weld, tol.value().tau_A});
            if (!m)
                return fail(assembly_error_name(m.error().message), m.error().message);
            merged = m.value().stack;
            header.welded_interfaces = m.value().welded_interfaces;
            defects.insert(defects.end(), m.value().defects.begin(), m.value().defects.end());
        }
        summarize_assembly_parts(base, merged, parts);
        auto policy = apply_repair_policy(defects, accepted, options.accept_b_tier);
        for (const auto& warning : policy.warnings)
            std::cerr << "Warning: " << warning << '\n';
        defects = policy.defects;
        if (std::any_of(
                defects.begin(), defects.end(), [](const Defect& d) { return d.tier == Tier::C; }))
            return fail("IMPORT_REJECTED", "IMPORT_REJECTED: C-tier defects");
        if (policy.result == ImportResult::Rejected)
            return fail("IMPORT_REJECTED", "IMPORT_REJECTED: repair policy rejected import");
        if (policy.result == ImportResult::ConfirmationRequired)
            return fail("IMPORT_CONFIRMATION_REQUIRED",
                        "IMPORT_CONFIRMATION_REQUIRED: B-tier defects require acceptance");
        if (auto id = floating_gap_part(merged, base, placed))
            return fail("INPUT_INVALID", "gap without bond leaves " + *id + " floating");
        // Keep the staged stack beside its sidecars so relative references
        // resolve identically during validation and after publication.
        auto staged = options.output;
        do {
            staged = options.output;
            staged += ".tmp." + std::to_string(std::random_device{}());
        } while (std::filesystem::exists(staged));
        Cleanup output_cleanup{staged};
        auto written = write_stack(merged, staged);
        if (!written)
            return fail("INPUT_INVALID", written.error().reason);
        auto validated = mesh::load_stack(staged, assembly_root() / "schema/stack-0.2.schema.json");
        if (!validated)
            return fail("INPUT_INVALID", validated.error().reason);
        reports("OK", "");
        std::filesystem::rename(staged, options.output);
        output_cleanup.p.clear();
        return 0;
#endif
    } catch (const std::exception& e) {
        auto message = std::string(e.what());
        try {
            return fail("INTERNAL", message);
        } catch (const std::exception& report_error) {
            std::cerr << report_error.what() << '\n';
            return 1;
        }
    }
}
} // namespace chipcae::importer
#else
namespace chipcae::importer {
int run_assembly(const ImportOptions& options) {
    write_defects(options.report, "INPUT_INVALID", "INPUT_INVALID: JSON support disabled");
    return 14;
}
} // namespace chipcae::importer
#endif
#endif
