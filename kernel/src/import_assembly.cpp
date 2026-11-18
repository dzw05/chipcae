#include "chipcae/geometry/assembly.hpp"
#include "chipcae/geometry/import_report.hpp"
#include "import_common.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <yaml-cpp/yaml.h>
namespace chipcae::importer {
namespace {
std::string defect_json_quote(const std::string& s) {
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
} // namespace

void write_defect(std::ostream& out, const geometry::Defect& d) {
    const auto id = geometry::defect_id(d), tier = geometry::tier_name(d.tier);
    out << "{\"defect_id\":" << defect_json_quote(id) << ",\"tier\":" << defect_json_quote(tier)
        << ",\"kind\":" << defect_json_quote(d.kind)
        << ",\"reason\":" << static_cast<unsigned>(d.reason)
        << ",\"dimension\":" << defect_json_quote(d.dimension)
        << ",\"size\":" << std::setprecision(17) << d.size
        << ",\"suggestion\":" << defect_json_quote(d.suggestion)
        << ",\"action\":" << defect_json_quote(d.action)
        << ",\"rejection_reason\":" << defect_json_quote(d.rejection_reason)
        << ",\"material_ambiguous\":" << (d.material_ambiguous ? "true" : "false");
    out << ",\"details\":" << defect_json_quote(d.details);
    out << '}';
}
void write_defects(const std::filesystem::path& file,
                   const std::string& code,
                   const std::string& message,
                   const std::vector<geometry::Defect>& defects) {
    if (file.empty())
        return;
    std::ofstream out(file);
    out << "{\"code\":" << defect_json_quote(code) << ",\"message\":" << defect_json_quote(message)
        << ",\"defects\":[";
    bool first = true;
    for (const auto& d : defects) {
        if (!first)
            out << ',';
        first = false;
        write_defect(out, d);
    }
    out << "]}\n";
    out.close();
    if (!out)
        throw std::runtime_error("cannot write defects output");
}
std::optional<int> protocol_error_code(const std::string& name, std::istream& input) {
    try {
        const auto document = YAML::Load(input);
        for (const auto& entry : document["errors"])
            if (entry["name"].as<std::string>() == name)
                return entry["code"].as<int>();
    } catch (const YAML::Exception&) {
    }
    return std::nullopt;
}

namespace {
using namespace geometry;
// Private assembly report extension.
struct ImportReportParts {
    std::map<std::string, std::vector<std::string>> split_layers;
    std::vector<std::pair<std::string, std::vector<std::string>>> rewrite_targets;
    std::vector<std::string> material_approx;
};

namespace {
std::string quote(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')
            out += '"';
        out += c;
    }
    return out + '"';
}
std::string json_quote(const std::string& s) {
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
void strings(std::ostream& out, const std::vector<std::string>& values) {
    out << '[';
    bool first = true;
    for (const auto& value : values) {
        if (!first)
            out << ',';
        first = false;
        out << json_quote(value);
    }
    out << ']';
}
} // namespace
inline bool write_assembly_report(const std::filesystem::path& dir,
                                  const ImportReportHeader& h,
                                  const std::vector<Defect>& ds,
                                  std::string* error,
                                  const ImportReportParts* parts) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        if (error)
            *error = ec.message();
        return false;
    }
    unsigned counts[4]{};
    for (const auto& d : ds)
        ++counts[static_cast<unsigned>(d.tier)];
    std::ofstream j(dir / "import_report.json"), c(dir / "import_report.csv");
    if (!j || !c) {
        if (error)
            *error = "cannot open report output";
        return false;
    }
    j << std::setprecision(17) << "{\"L\":" << h.L << ",\"t_min\":" << h.t_min
      << ",\"eps_weld\":" << h.tolerances.eps_weld << ",\"tau_A\":" << h.tolerances.tau_A
      << ",\"tau_B\":" << h.tolerances.tau_B << ",\"delta_f\":" << h.tolerances.delta_f
      << ",\"tier_counts\":{\"Weld\":" << counts[0] << ",\"A\":" << counts[1]
      << ",\"B\":" << counts[2] << ",\"C\":" << counts[3]
      << "},\"b_tier_accepted\":" << (h.b_tier_accepted_all ? "\"all\"" : "null")
      << ",\"defects\":[";
    c << "defect_id,tier,kind,reason,dimension,size,suggestion,action,rejection_reason,material_"
         "ambiguous\n";
    bool first = true;
    for (const auto& d : ds) {
        const auto id = defect_id(d), tier = tier_name(d.tier);
        if (!first)
            j << ',';
        first = false;
        write_defect(j, d);
        c << quote(id) << ',' << quote(tier) << ',' << quote(d.kind) << ','
          << static_cast<unsigned>(d.reason) << ',' << quote(d.dimension) << ','
          << std::setprecision(17) << d.size << ',' << quote(d.suggestion) << ',' << quote(d.action)
          << ',' << quote(d.rejection_reason) << ',' << (d.material_ambiguous ? "true" : "false")
          << '\n';
    }
    j << ']';
    if (parts) {
        j << ",\"parts\":{\"split_layers\":{";
        bool first = true;
        for (const auto& [name, targets] : parts->split_layers) {
            if (!first)
                j << ',';
            first = false;
            j << json_quote(name) << ':';
            strings(j, targets);
        }
        j << "},\"rewrite_targets\":[";
        first = true;
        for (const auto& [from, targets] : parts->rewrite_targets) {
            if (!first)
                j << ',';
            first = false;
            j << "{\"from\":" << json_quote(from) << ",\"to\":";
            strings(j, targets);
            j << '}';
        }
        j << "],\"material_approx\":[";
        first = true;
        for (const auto& id : parts->material_approx) {
            if (!first)
                j << ',';
            first = false;
            j << "{\"id\":" << json_quote(id)
              << ",\"material_approx\":true,\"label\":\"近似材料\"}";
        }
        j << "]}";
    }
    j << "}\n";
    j.close();
    c.close();
    if (!j || !c) {
        if (error)
            *error = "cannot write report output";
        return false;
    }
    return true;
}

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
    return true;
}
std::vector<std::string> interface_rewrite_targets(const std::vector<std::string>& lower,
                                                   const std::vector<std::string>& upper) {
    std::vector<std::string> targets;
    for (const auto& a : lower)
        for (const auto& b : upper)
            targets.push_back("interface:" + a + "|" + b);
    return targets;
}
int assembly_error_exit(const std::string& message, std::istream& in) {
    return protocol_error_code(message.substr(0, message.find(':')), in).value_or(1);
}
int assembly_error_exit(const std::string& message) {
    std::ifstream in(assembly_root() / "protocol/errors.yaml");
    return assembly_error_exit(message, in);
}
} // namespace chipcae::importer
#ifdef CHIPCAE_ASSEMBLY_CLI
#include "stack_writer.hpp"
#if defined(CHIPCAE_ENABLE_JSONCONS)
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#ifdef CHIPCAE_ENABLE_ECAD
#include "chipcae/geometry/ecad/ipc2581.hpp"
#ifdef CHIPCAE_ASSEMBLY_ODBPP_AVAILABLE
#include "chipcae/geometry/ecad/odbpp.hpp"
#endif
#endif
namespace chipcae::importer {
int run_assembly(const ImportOptions& options) {
    using namespace geometry;
    using J = jsoncons::json;
    std::vector<Defect> defects;
    ImportReportHeader header;
    ImportReportParts parts;
    auto reports = [&](const std::string& code, const std::string& message) {
        std::string error;
        const auto dir = import_report_directory(options.output);
        if (!write_assembly_report(dir, header, defects, &error, &parts))
            throw std::runtime_error(error);
        write_defects(options.report, code, message, defects);
    };
    auto fail = [&](int exit, const std::string& code, const std::string& message) {
        const auto full =
            message.starts_with(code + ": ") || std::regex_search(message, std::regex("^[A-Z_]+: "))
                ? message
                : code + ": " + message;
        const auto prefix = full.substr(0, full.find(':'));
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
            return fail(14, "INPUT_INVALID", "INPUT_INVALID: polyset must be in output directory");
        J doc;
        try {
            std::ifstream in(options.assembly);
            if (!in)
                throw std::runtime_error("cannot open assembly");
            doc = J::parse(in);
        } catch (const std::exception& e) {
            return fail(14,
                        "INPUT_INVALID",
                        "INPUT_INVALID: assembly=" + options.assembly.string() +
                            "; json_path=$: " + e.what());
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
            return fail(
                7, "IMPORT_REJECTED", std::string("IMPORT_REJECTED: runtime assets: ") + e.what());
        }
        if (!invalid.empty())
            return fail(14, "INPUT_INVALID", invalid);
        const auto root = options.assembly.parent_path();
        const auto board_format = doc["board"]["format"].as<std::string>();
        const auto board_path = std::filesystem::path(doc["board"]["path"].as<std::string>());
        if (board_format != "ipc2581" && board_format != "odbpp")
            return fail(14, "INPUT_INVALID", "json_path=$/board/format: unsupported board format");
        const auto resolved_board = std::filesystem::weakly_canonical(root / board_path);
        const auto relative_board = resolved_board.lexically_relative(
            std::filesystem::weakly_canonical(root.empty() ? "." : root));
        if (board_path.empty() || board_path.is_absolute() || relative_board.empty() ||
            *relative_board.begin() == ".." ||
            (board_format == "ipc2581" ? !std::filesystem::is_regular_file(resolved_board)
                                       : !std::filesystem::exists(resolved_board)))
            return fail(
                14, "INPUT_INVALID", "json_path=$/board/path: invalid or missing board path");
        std::set<std::string> ids;
        for (const auto& p : doc["parts"].array_range()) {
            auto id = p["id"].as<std::string>();
            if (id == "void")
                return fail(14, "INPUT_INVALID", "INPUT_INVALID: part id void is reserved");
            if (!ids.insert(id).second)
                return fail(7, "IMPORT_REJECTED", "duplicate part id " + id);
            if (p["step"].as<std::string>().find("NOT_CAE_GEOMETRY") != std::string::npos)
                return fail(7,
                            "IMPORT_REJECTED",
                            "IMPORT_REJECTED: display-only geometry NOT_CAE_GEOMETRY");
        }
        std::vector<std::string> accepted;
        auto accept = options.accept_file;
        if (accept.empty() && !options.accept_b_tier) {
            auto candidate = root / "import.accept.json";
            if (std::filesystem::exists(candidate))
                accept = candidate;
        }
        if (!accept.empty()) {
            try {
                std::ifstream in(accept);
                auto a = J::parse(in);
                for (const auto& item : a["accept"].array_range())
                    accepted.push_back(item["defect_id"].as<std::string>());
            } catch (const std::exception& e) {
                return fail(14, "INPUT_INVALID", std::string("accept: ") + e.what());
            }
        }
#ifndef CHIPCAE_ENABLE_ECAD
        return fail(7, "IMPORT_REJECTED", "ECAD support disabled");
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
            return fail(7, "IMPORT_REJECTED", board.error);
        std::string error;
        if (!ecad::apply_material_map(
                board.model, root / b["material_map"].as<std::string>(), &error))
            return fail(9, "MATERIAL_MAPPING_MISSING", error);
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
            return fail(7, "IMPORT_REJECTED", error);
        auto loaded = mesh::load_stack(temp, assembly_root() / "schema/stack-0.2.schema.json");
        if (!loaded)
            return fail(14, "INPUT_INVALID", loaded.error().reason);
        auto base = loaded.value();
        auto poly = mesh::read_polyset(options.polyset);
        if (!poly)
            return fail(14, "INPUT_INVALID", poly.error().reason);
        auto outline = inline_assembly_outline(base, poly.value());
        if (!outline)
            return fail(13, "CUT_UNSUPPORTED", outline.error().reason);
        ImportScales scales;
        scales.L = std::max(base.outline.box[2] - base.outline.box[0],
                            base.outline.box[3] - base.outline.box[1]);
        if (base.layers.empty())
            return fail(14, "INPUT_INVALID", "board has no layers");
        scales.t_min = base.layers.front().thickness;
        for (const auto& l : base.layers)
            scales.t_min = std::min(scales.t_min, l.thickness);
        scales.h_xy = scales.L / 100;
        scales.dbu_m = poly.value().dbu_m;
        auto tol = compute_tolerances(scales, {});
        if (!tol)
            return fail(7, "IMPORT_REJECTED", tol.error().message);
        header.L = scales.L;
        header.t_min = scales.t_min;
        header.tolerances = tol.value();
        header.b_tier_accepted_all = options.accept_b_tier;
        auto read_step = [&](const J& p) {
            StepImportOptions io;
            io.tolerances = tol.value();
            try {
                return import_step(root / p["step"].as<std::string>(), io);
            } catch (const std::exception& e) {
                throw std::runtime_error(std::string("IMPORT_REJECTED: ") + e.what());
            }
        };
        std::vector<J> all_parts;
        std::map<std::string, StepImportResult> shared_steps;
        for (const auto& p : doc["parts"].array_range())
            all_parts.push_back(p);
        if (doc.contains("parts_from_step")) {
            const auto& f = doc["parts_from_step"];
            for (const auto& kv : f["bodies"].object_range()) {
                auto id = std::string(kv.key());
                if (id == "void")
                    return fail(14, "INPUT_INVALID", "INPUT_INVALID: part id void is reserved");
                if (!ids.insert(id).second)
                    return fail(7, "IMPORT_REJECTED", "duplicate part id " + id);
            }
            if (f["step"].as<std::string>().find("NOT_CAE_GEOMETRY") != std::string::npos)
                return fail(7, "IMPORT_REJECTED", "IMPORT_REJECTED: display-only geometry");
            std::optional<StepImportResult> imported;
            std::vector<std::string> names;
            if (step_import_available()) {
                imported = read_step(f);
                std::set<std::string> unique;
                for (const auto& name : imported->body_names) {
                    if (!unique.insert(name).second)
                        return fail(7, "IMPORT_REJECTED", "duplicate body name " + name);
                    if (!f["bodies"].contains(name)) {
                        Defect d{};
                        d.tier = Tier::B;
                        d.reason = DefectReason::PART_UNMAPPED;
                        d.defect_id = "unmapped/" + name;
                        defects.push_back(d);
                    } else
                        names.push_back(name);
                }
            } else
                for (const auto& kv : f["bodies"].object_range())
                    names.emplace_back(kv.key());
            for (const auto& name : names) {
                J p = f["bodies"][name];
                p["id"] = name;
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
                            return fail(7,
                                        "IMPORT_REJECTED",
                                        "IMPORT_REJECTED: missing attach target " + target);
                        }
                    } else if (!done.contains(target.substr(5))) {
                        if (std::none_of(pending.begin(), pending.end(), [&](const J& part) {
                                return part["id"].as<std::string>() == target.substr(5);
                            })) {
                            attachment_defect(id, "missing attach target " + target);
                            return fail(7,
                                        "IMPORT_REJECTED",
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
                return fail(7, "IMPORT_REJECTED", "IMPORT_REJECTED: cyclic part attachments");
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
                if (n.find("NOT_CAE_GEOMETRY") != std::string::npos)
                    throw std::runtime_error("IMPORT_REJECTED: display-only geometry");
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
            SliceResult sliced;
            try {
                sliced = slice_prismatic(
                    step, body, {tol.value().tau_A, tol.value().tau_A, tol.value().tau_B});
            } catch (const std::logic_error& e) {
                if (std::string(e.what()) != "not implemented: slice_prismatic")
                    throw;
            }
            for (auto d : sliced.defects) {
                d.defect_id = id + "/" + d.defect_id;
                defects.push_back(d);
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
                }
                placement = a;
            } else
                placement = FramePlacement{v["origin"].as<std::array<Real, 3>>()};
            try {
                auto result = place(sliced.slabs, placement, base);
                if (!result)
                    throw std::runtime_error(result.error().message);
                auto pp = result.value();
                pp.id = id;
                pp.material = p["material"].as<std::string>();
                placed.push_back(pp);
            } catch (const std::logic_error& e) {
                if (std::string(e.what()) != "not implemented: place")
                    throw;
            }
        };
        for (const auto& p : ordered) {
            auto shared = shared_steps.find(p["id"].as<std::string>());
            process(p, shared == shared_steps.end() ? nullptr : &shared->second);
        }
        auto merged = base;
        if (!placed.empty()) {
            try {
                auto m = merge_parts(
                    base, placed, {scales.t_min, tol.value().eps_weld, tol.value().tau_A});
                if (!m)
                    return fail(assembly_error_exit(m.error().message),
                                assembly_error_exit(m.error().message) == 1
                                    ? "INTERNAL"
                                    : m.error().message.substr(0, m.error().message.find(':')),
                                m.error().message);
                merged = m.value().stack;
                defects.insert(defects.end(), m.value().defects.begin(), m.value().defects.end());
            } catch (const std::logic_error& e) {
                if (std::string(e.what()) != "not implemented: merge_parts")
                    throw;
            }
        }
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
                rewrite("layer:" + l.name, targets);
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
        auto policy = apply_repair_policy(defects, accepted, options.accept_b_tier);
        for (const auto& warning : policy.warnings)
            std::cerr << "Warning: " << warning << '\n';
        defects = policy.defects;
        if (std::any_of(
                defects.begin(), defects.end(), [](const Defect& d) { return d.tier == Tier::C; }))
            return fail(7, "IMPORT_REJECTED", "IMPORT_REJECTED: C-tier defects");
        if (policy.result == ImportResult::Rejected)
            return fail(7, "IMPORT_REJECTED", "IMPORT_REJECTED: repair policy rejected import");
        if (policy.result == ImportResult::ConfirmationRequired)
            return fail(6,
                        "IMPORT_CONFIRMATION_REQUIRED",
                        "IMPORT_CONFIRMATION_REQUIRED: B-tier defects require acceptance");
        Cleanup output_cleanup{options.output};
        auto written = write_stack(merged, options.output);
        if (!written)
            return fail(14, "INPUT_INVALID", written.error().reason);
        auto validated =
            mesh::load_stack(options.output, assembly_root() / "schema/stack-0.2.schema.json");
        if (!validated)
            return fail(14, "INPUT_INVALID", validated.error().reason);
        reports("OK", "");
        output_cleanup.p.clear();
        return 0;
#endif
    } catch (const std::exception& e) {
        auto message = std::string(e.what());
        auto exit = assembly_error_exit(message);
        try {
            return fail(
                exit, exit == 1 ? "INTERNAL" : message.substr(0, message.find(':')), message);
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
