#include "assembly_seam_support.hpp"
#include "writer_test_support.hpp"

#include <chrono>
#include <set>

int main() {
    using namespace chipcae;
    using sp4_test::require;
    try {
        geometry::StepDefect open;
        open.defect_id = "shell#1";
        open.kind = geometry::StepDefectKind::OpenShell;
        open.tier = geometry::Tier::C;
        open.measured = .0000123456789012345;
        open.step_entity = "#123";
        open.body_name = "solid/lid";
        open.face_index = 9;
        geometry::StepDefect face = open;
        face.defect_id = "face#1";
        face.kind = geometry::StepDefectKind::FlippedFace;
        face.tier = geometry::Tier::B;
        face.status = "BadOrientation";
        auto incomplete = face;
        incomplete.status = "InspectionIncomplete";
        const std::vector<geometry::StepDefect> input{open, face, incomplete};
        const auto mapped = importer::map_step_defects("lid", input);
        require(mapped.size() == 3, "map_step_defects must preserve all three STEP defects");
        std::set<std::string> ids;
        for (std::size_t i = 0; i < mapped.size(); ++i) {
            const auto& d = mapped[i];
            require(ids.insert(d.defect_id).second, "duplicate mapped defect id");
            require(d.defect_id.starts_with("lid/"), "missing part id prefix");
            require(d.kind == (i == 0 ? "step.OpenShell" : "step.FlippedFace"), "kind mapping");
            require(d.tier == input[i].tier, "tier changed");
            require(static_cast<unsigned>(d.reason) == 0, "reason must be integer zero");
            require(d.dimension == "measured", "measured dimension lost");
            sp4_test::real(input[i].measured, d.size);
            require(d.details == "step_entity=#123;body=solid/lid;face=9;status=" + input[i].status,
                    "diagnostic details changed");
        }
        require(mapped[0].defect_id == "lid/shell#1" && mapped[1].defect_id == "lid/face#1" &&
                    mapped[2].defect_id == "lid/face#1#InspectionIncomplete",
                "status suffix must only be added for duplicate IDs");
        auto other = open;
        other.body_name = "solid/other";
        auto global = face;
        global.body_name.clear();
        const auto filtered = importer::map_step_defects("lid", {open, other, global}, "solid/lid");
        require(filtered.size() == 2, "shared STEP defects must match body or be global");
        require(filtered[0].details.find("body=solid/lid;") != std::string::npos &&
                    filtered[1].details.find("body=;") != std::string::npos,
                "wrong body diagnostic survived filtering");
        const auto repeated = importer::map_step_defects("lid", {face, incomplete, incomplete});
        require(repeated[1].defect_id != repeated[2].defect_id,
                "repeated statuses must still have unique IDs");
        const auto dir =
            std::filesystem::temp_directory_path() /
            ("sp4-mapping-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        importer::ImportReportParts parts;
        mesh::StackModel base, merged;
        mesh::Layer layer;
        layer.name = "L";
        base.layers.push_back(layer);
        layer.name = "L#0";
        merged.layers.push_back(layer);
        layer.name = "L#1";
        merged.layers.push_back(layer);
        importer::summarize_assembly_parts(base, merged, parts);
        require(parts.split_layers.at("L") == std::vector<std::string>{"L#0", "L#1"},
                "synthetic layer split must be derived without slicing");
        require(parts.rewrite_targets ==
                    std::vector<std::pair<std::string, std::vector<std::string>>>{
                        {"layer:L", {"layer:L#0", "layer:L#1"}}},
                "synthetic layer rewrite must be derived without slicing");
        const std::string special = "layer\"\\\n";
        parts.split_layers[special] = {special + "#0", special + "#1"};
        parts.rewrite_targets.emplace_back("layer:" + special, parts.split_layers[special]);
        parts.material_approx.push_back(special);
        require(importer::write_assembly_report(dir, {}, mapped, nullptr, &parts),
                "report write failed");
#ifdef SP4_HAS_JSONCONS
        require(geometry::write_import_report(dir / "shared", {}, mapped),
                "shared report write failed");
        const auto shared =
            jsoncons::json::parse(sp4_test::bytes(dir / "shared/import_report.json"));
        auto assembly = jsoncons::json::parse(sp4_test::bytes(dir / "import_report.json"));
        assembly.erase("parts");
        for (auto& d : assembly["defects"].array_range())
            d.erase("details");
        require(assembly == shared, "assembly must preserve all shared report fields");
        require(sp4_test::bytes(dir / "import_report.csv") ==
                    sp4_test::bytes(dir / "shared/import_report.csv"),
                "assembly CSV must be byte-identical to shared CSV");
        const auto json = jsoncons::json::parse(sp4_test::bytes(dir / "import_report.json"));
        require(json["parts"]["split_layers"][special].as<std::vector<std::string>>() ==
                    parts.split_layers[special],
                "split layer extension roundtrip");
        require(json["parts"]["rewrite_targets"][1]["from"].as<std::string>() == "layer:" + special,
                "rewrite extension roundtrip");
        require(json["parts"]["material_approx"][0]["id"].as<std::string>() == special,
                "material extension roundtrip");
        require(json.at("defects").size() == 3, "report roundtrip count");
        importer::write_defects(dir / "defects.json", "IMPORT_REJECTED", special, mapped);
        const auto defects_json = jsoncons::json::parse(sp4_test::bytes(dir / "defects.json"));
        require(defects_json["defects"] == shared["defects"],
                "defects output must preserve shared fields without details");
        require(defects_json["message"].as<std::string>() == special,
                "assembly defects writer must escape messages");
        importer::write_defects({}, "OK", "", mapped);

        for (std::size_t i = 0; i < mapped.size(); ++i) {
            const auto& d = json.at("defects").at(i);
            require(d.at("defect_id").as<std::string>() == mapped[i].defect_id, "report id lost");
            require(d.at("kind").as<std::string>() == mapped[i].kind, "report kind lost");
            require(d.at("tier").as<std::string>() == geometry::tier_name(mapped[i].tier),
                    "report tier lost");
            sp4_test::real(mapped[i].size, d.at("size").as<double>());
            require(d.at("dimension").as<std::string>() == "measured", "report dimension lost");
            require(d.at("reason").as<unsigned>() == 0, "report reason lost");
            require(d.at("details").as<std::string>() == mapped[i].details, "report details lost");
        }
#endif
        std::filesystem::remove_all(dir);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
