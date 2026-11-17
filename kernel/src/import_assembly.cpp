#include "chipcae/geometry/assembly.hpp"
#include "chipcae/geometry/import_report.hpp"
#include "import_common.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
namespace chipcae::importer {
namespace {
std::filesystem::path assembly_root() {
    return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
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
std::vector<geometry::Defect> map_step_defects(const std::string& part,
                                               const std::vector<geometry::StepDefect>& input) {
    std::vector<geometry::Defect> out;
    std::set<std::string> ids;
    for (const auto& s : input) {
        geometry::Defect d{};
        d.tier = s.tier;
        d.reason = geometry::DefectReason::TOLERANCE_RANGE_UNSUPPORTED;
        d.kind = std::string("step.") + step_kind(s.kind);
        d.dimension = "measured";
        d.size = s.measured;
        d.defect_id = part + "/" + s.defect_id;
        if (ids.contains(d.defect_id) && !s.status.empty())
            d.defect_id += "#" + s.status;
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
    for (const auto& l : poly.layers)
        if (l.name == "outline") {
            if (l.ring_offsets.size() != 2)
                return mesh::Error{"$/outline", "CUT_UNSUPPORTED: outline needs one ring"};
            for (auto i = l.ring_offsets[0]; i < l.ring_offsets[1]; ++i)
                stack.outline.polygon.push_back(
                    {static_cast<double>(l.coordinates.at(static_cast<std::size_t>(2 * i))) *
                         poly.dbu_m,
                     static_cast<double>(l.coordinates.at(static_cast<std::size_t>(2 * i + 1))) *
                         poly.dbu_m});
        }
    return true;
}
int assembly_error_exit(const std::string& message) {
    const auto name = message.substr(0, message.find(':'));
    std::ifstream in(assembly_root() / "protocol/errors.yaml");
    std::string line;
    const std::regex entry(R"(code:\s*([0-9]+),\s*name:\s*([A-Z_]+))");
    while (std::getline(in, line)) {
        std::smatch match;
        if (std::regex_search(line, match, entry) && match[2] == name)
            return std::stoi(match[1]);
    }
    return 1;
}
} // namespace chipcae::importer
#ifdef CHIPCAE_ASSEMBLY_CLI
#include "stack_writer.hpp"
#if defined(CHIPCAE_ENABLE_JSONCONS)
#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>
#ifdef CHIPCAE_ENABLE_ECAD
#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "chipcae/geometry/ecad/odbpp.hpp"
#endif
namespace chipcae::importer {
int run_assembly(const ImportOptions& options) {
    using namespace geometry;
    using J = jsoncons::json;
    std::vector<Defect> defects;
    ImportReportHeader header;
    J parts;
    parts["split_layers"] = J::object();
    parts["rewrite_targets"] = J::array();
    parts["material_approx"] = J::array();
    auto reports = [&](const std::string& code, const std::string& message) {
        std::string error;
        const auto dir = import_report_directory(options.output);
        if (!write_import_report(dir, header, defects, &error))
            throw std::runtime_error(error);
        J report;
        report["L"] = header.L;
        report["t_min"] = header.t_min;
        report["eps_weld"] = header.tolerances.eps_weld;
        report["tau_A"] = header.tolerances.tau_A;
        report["tau_B"] = header.tolerances.tau_B;
        report["delta_f"] = header.tolerances.delta_f;
        report["b_tier_accepted"] = header.b_tier_accepted_all ? J("all") : J::null();
        for (auto tier : {Tier::Weld, Tier::A, Tier::B, Tier::C})
            report["tier_counts"][tier_name(tier)] = 0;
        report["defects"] = J::array();
        for (const auto& defect : defects) {
            J d;
            d["defect_id"] = defect_id(defect);
            d["tier"] = tier_name(defect.tier);
            d["kind"] = defect.kind;
            d["reason"] = static_cast<unsigned>(defect.reason);
            d["dimension"] = defect.dimension;
            d["size"] = defect.size;
            d["suggestion"] = defect.suggestion;
            d["action"] = defect.action;
            d["rejection_reason"] = defect.rejection_reason;
            d["material_ambiguous"] = defect.material_ambiguous;
            d["details"] = defect.details;
            report["defects"].push_back(d);
            auto tier = tier_name(defect.tier);
            report["tier_counts"][tier] = report["tier_counts"][tier].as<unsigned>() + 1;
        }
        report["parts"] = parts;
        std::ofstream out(dir / "import_report.json");
        out << report << '\n';
        J d;
        d["code"] = code;
        d["message"] = message;
        d["defects"] = report["defects"];
        std::ofstream dest(options.report);
        dest << d << '\n';
        if (!out || !dest)
            throw std::runtime_error("cannot write assembly reports");
    };
    auto fail = [&](int exit, const std::string& code, const std::string& message) {
        reports(code, message);
        std::cerr << message << '\n';
        return exit;
    };
    try {
        std::filesystem::create_directories(import_report_directory(options.output));
        if (std::filesystem::weakly_canonical(
                options.polyset.parent_path().empty() ? "." : options.polyset.parent_path()) !=
            std::filesystem::weakly_canonical(import_report_directory(options.output)))
            return fail(14, "INPUT_INVALID", "INPUT_INVALID: polyset must be in output directory");
        J doc;
        try {
            std::ifstream in(options.assembly);
            if (!in)
                throw std::runtime_error("cannot open assembly");
            doc = J::parse(in);
        } catch (const std::exception& e) {
            return fail(14, "INPUT_INVALID", std::string("json_path=$: ") + e.what());
        }
        for (auto key : {"schema", "board", "parts"})
            if (!doc.contains(key))
                return fail(14, "INPUT_INVALID", std::string("json_path=$/") + key + ": required");
        std::ifstream spec(assembly_root() / "schema/assembly-0.1.schema.json");
        auto validator = jsoncons::jsonschema::make_json_schema(J::parse(spec));
        std::string invalid;
        validator.validate(doc, [&](const jsoncons::jsonschema::validation_message& m) {
            if (invalid.empty())
                invalid = "json_path=$" + m.instance_location().string() + ": " + m.message();
            return jsoncons::jsonschema::walk_result::advance;
        });
        if (!invalid.empty())
            return fail(7, "IMPORT_REJECTED", invalid);
        const auto root = options.assembly.parent_path();
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
        auto board = b["format"].as<std::string>() == "odbpp"
                         ? ecad::read_odbpp(root / b["path"].as<std::string>())
                         : ecad::read_ipc2581(root / b["path"].as<std::string>());
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
                auto id = kv.value()["id"].as<std::string>();
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
        std::map<std::string, J> pending;
        for (const auto& p : all_parts)
            pending.emplace(p["id"].as<std::string>(), p);
        std::vector<J> ordered;
        std::set<std::string> done;
        while (!pending.empty()) {
            bool progress = false;
            for (auto it = pending.begin(); it != pending.end();) {
                const auto& v = it->second["placement"];
                if (v.contains("attach")) {
                    auto ref = v["attach"].as<std::string>();
                    auto target = ref.substr(0, ref.rfind('@'));
                    if (target.starts_with("layer:")) {
                        auto name = target.substr(6);
                        if (std::none_of(base.layers.begin(),
                                         base.layers.end(),
                                         [&](const mesh::Layer& l) { return l.name == name; })) {
                            attachment_defect(it->first, "missing attach target " + target);
                            return fail(7,
                                        "IMPORT_REJECTED",
                                        "IMPORT_REJECTED: missing attach target " + target);
                        }
                    } else if (!done.contains(target.substr(5))) {
                        if (!pending.contains(target.substr(5))) {
                            attachment_defect(it->first, "missing attach target " + target);
                            return fail(7,
                                        "IMPORT_REJECTED",
                                        "IMPORT_REJECTED: missing attach target " + target);
                        }
                        ++it;
                        continue;
                    }
                }
                done.insert(it->first);
                ordered.push_back(it->second);
                it = pending.erase(it);
                progress = true;
            }
            if (!progress) {
                attachment_defect(pending.begin()->first, "cyclic part attachments");
                return fail(7, "IMPORT_REJECTED", "IMPORT_REJECTED: cyclic part attachments");
            }
        }
        std::vector<PlacedPart> placed;
        auto process = [&](const J& p, const StepImportResult* shared = nullptr) {
            auto id = p["id"].as<std::string>();
            if (id == "void")
                throw std::runtime_error("INPUT_INVALID: part id void is reserved");
            for (const auto& l : base.layers) {
                if (id == l.name || id == l.body)
                    throw std::runtime_error("INPUT_INVALID: part id collides with " + id);
                for (const auto& r : l.regions)
                    if (id == r.name || id == r.body)
                        throw std::runtime_error("INPUT_INVALID: part id collides with " + id);
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
            auto mapped = map_step_defects(id, step.defects);
            defects.insert(defects.end(), mapped.begin(), mapped.end());
            std::set<std::string> names;
            for (const auto& n : step.body_names) {
                if (n.find("NOT_CAE_GEOMETRY") != std::string::npos)
                    throw std::runtime_error("IMPORT_REJECTED: display-only geometry");
                if (!names.insert(n).second)
                    throw std::runtime_error("IMPORT_REJECTED: duplicate body name " + n);
            }
            if (p.get_value_or<bool>("material_approx", false)) {
                J approx;
                approx["id"] = id;
                approx["material_approx"] = true;
                approx["label"] = "近似材料";
                parts["material_approx"].push_back(approx);
            }
            SliceResult sliced;
            try {
                sliced = slice_prismatic(
                    step,
                    p.get_value_or<std::string>(
                        "body", step.body_names.empty() ? "" : step.body_names.front()),
                    {tol.value().tau_A, tol.value().tau_A, tol.value().tau_B});
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
            J r;
            r["from"] = from;
            r["to"] = targets;
            parts["rewrite_targets"].push_back(r);
        };
        std::map<std::string, std::vector<std::string>> layer_targets;
        for (const auto& l : base.layers) {
            std::vector<std::string> split, targets;
            for (const auto& n : merged.layers)
                if (n.name == l.name || n.name.starts_with(l.name + "#")) {
                    targets.push_back("layer:" + n.name);
                    if (n.name != l.name)
                        split.push_back(n.name);
                }
            layer_targets[l.name] = targets;
            if (!split.empty()) {
                parts["split_layers"][l.name] = split;
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
            auto target = "interface:" + layer_targets[a].back().substr(6) + "|" +
                          layer_targets[b].front().substr(6);
            auto from = "interface:" + a + "|" + b;
            if (target != from)
                rewrite(from, {target});
        }
        auto policy = apply_repair_policy(defects, accepted, options.accept_b_tier);
        for (const auto& warning : policy.warnings)
            std::cerr << "Warning: " << warning << '\n';
        defects = policy.defects;
        if (std::any_of(
                defects.begin(), defects.end(), [](const Defect& d) { return d.tier == Tier::C; }))
            return fail(7, "IMPORT_REJECTED", "IMPORT_REJECTED: C-tier defects");
        if (policy.result == ImportResult::ConfirmationRequired)
            return fail(6, "IMPORT_CONFIRMATION_REQUIRED", "IMPORT_CONFIRMATION_REQUIRED");
        reports("OK", "");
        auto written = write_stack(merged, options.output);
        if (!written)
            return fail(14, "INPUT_INVALID", written.error().reason);
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
    write_defects(options.report, "IMPORT_REJECTED", "JSON support disabled");
    return 7;
}
} // namespace chipcae::importer
#endif
#endif
