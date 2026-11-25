#define CHIPCAE_ENABLE_ECAD 1
#include "../../../kernel/src/import_ecad_defects.hpp"
#include "../../../schema/src/sha256.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
namespace ecad = chipcae::geometry::ecad;
namespace geometry = chipcae::geometry;
const std::filesystem::path root = CHIPCAE_SOURCE_DIR;
constexpr auto baseline_sha = "51600d4a9e89521e0874e5933c4bed8a293e512d";
constexpr auto metadata_action = "non-physical metadata ignored";
constexpr auto definition_action = "definition expressed by modeled instances";
constexpr auto confirm = "confirm omitted metadata, components or text";
struct Temporary {
    std::filesystem::path path;
    Temporary() {
        std::random_device random;
        do {
            path = std::filesystem::temp_directory_path() /
                   ("metadata-tier-" + std::to_string(random()) + "-" + std::to_string(random()));
        } while (!std::filesystem::create_directory(path));
    }
    ~Temporary() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path write(const std::string& name, const std::string& text) const {
        const auto file = path / name;
        std::ofstream out(file, std::ios::binary);
        out << text;
        if (!out)
            throw std::runtime_error("cannot write " + name);
        return file;
    }
};
std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read " + path.filename().string());
    return {(std::istreambuf_iterator<char>(input)), {}};
}
struct Case {
    std::string name, xml;
    bool fixture = false;
};
// One dbu is one micron. Polygons use integer millimeters unless a guard says otherwise.
std::string polygon(const std::string& maximum = "4") {
    return "<Polygon><PolyBegin x=\"0\" y=\"0\"/><PolyStepSegment x=\"" + maximum +
           "\" y=\"0\"/><PolyStepSegment x=\"" + maximum +
           "\" y=\"3\"/><PolyStepSegment x=\"0\" y=\"3\"/></Polygon>";
}
std::string board(const std::string& step,
                  const std::string& top = "",
                  const std::string& head = "",
                  const std::string& layers = "",
                  bool profile = true,
                  const std::string& units = "MILLIMETER") {
    return "<IPC-2581>\n" + top + "\n<CadHeader units=\"" + units + "\">" + head +
           "</CadHeader>\n<CadData>\n"
           "<Layer name=\"Cu\" layerFunction=\"CONDUCTOR\"/>\n" +
           layers +
           "\n<Stackup><StackupGroup><StackupLayer layerOrGroupRef=\"Cu\" thickness=\"0.035\" "
           "matDes=\"FR4\"/></StackupGroup></Stackup>\n"
           "<Step name=\"metadata\">\n" +
           (profile ? "<Profile>" + polygon() + "</Profile>\n" : "") + step +
           "\n</Step></CadData></IPC-2581>\n";
}
std::string features(const std::string& shape) {
    return "<LayerFeature layerRef=\"Doc\"><Set><Features>" + shape +
           "</Features></Set></LayerFeature>";
}
const std::string disk = "<DictionaryStandard><EntryStandard id=\"disk\"><Circle diameter=\"1\"/>"
                         "</EntryStandard></DictionaryStandard>";
const std::string package =
    "<Package name=\"P\"><Outline>" + polygon() +
    "<LineDescRef id=\"line\"/></Outline><PickupPoint x=\"0\" y=\"0\"/><OtherSideView/>"
    "<Pin number=\"1\"><Location x=\"0\" y=\"0\"/><StandardPrimitiveRef id=\"disk\"/>"
    "</Pin></Package>";
const std::string placed =
    "<Component refDes=\"J1\" packageRef=\"P\"><Location x=\"2\" y=\"2\"/></Component>";
const std::string padstack = "<PadStackDef name=\"PS\"><PadstackPadDef layerRef=\"Cu\">"
                             "<StandardPrimitiveRef id=\"disk\"/><UserPrimitiveRef id=\"user\"/>"
                             "</PadstackPadDef></PadStackDef>";
const std::string dictionaries =
    "<DictionaryUser><EntryUser id=\"user\"><UserSpecial><Circle diameter=\"1\"/>"
    "</UserSpecial></EntryUser></DictionaryUser>"
    "<DictionaryLineDesc><EntryLineDesc id=\"line\"><LineDesc lineWidth=\"0\"/>"
    "</EntryLineDesc></DictionaryLineDesc>";
std::string pad(const std::string& component = "J1",
                const std::string& pin = "1",
                const std::string& stack = "PS",
                const std::string& primitive = "disk",
                const std::string& layer = "Cu") {
    return "<LayerFeature layerRef=\"" + layer + "\"><Set><Pad padstackDefRef=\"" + stack +
           "\"><Location x=\"2\" y=\"2\"/><StandardPrimitiveRef id=\"" + primitive +
           "\"/><PinRef componentRef=\"" + component + "\" pin=\"" + pin +
           "\"/></Pad></Set></LayerFeature>";
}
std::vector<Case> cases() {
    std::vector<Case> all;
    const auto add = [&](std::string name, std::string xml) {
        all.push_back({std::move(name), std::move(xml)});
    };
    add("netlist", board(pad(), disk));
    add("content",
        board("",
              "<Content><LayerRef/><StepRef/><FunctionMode/><BomRef/>"
              "<AvlRef/></Content><Bom><StepRef/></Bom>"));
    add("bom",
        board("",
              "<Bom><BomHeader/><BomItem><RefDes/><Characteristics><Textual/>"
              "</Characteristics></BomItem></Bom>"
              "<Avl><AvlHeader/><AvlItem/></Avl>"));
    add("header",
        board("",
              "<LogisticHeader><Role/><Enterprise/><Person/></LogisticHeader>"
              "<HistoryRecord><FileRevision><SoftwarePackage><Certification/>"
              "</SoftwarePackage></FileRevision></HistoryRecord>"));
    add("step_attr", board("<NonstandardAttribute name=\"FOOTPRINT_COUNT\" value=\"1\"/>"));
    for (const auto* units : {"MILLIMETER", "MICRON", "INCH"}) {
        add(std::string("datum_") + units,
            board("<Datum x=\"-0.0\" y=\"0e3\"/>", "", "", "", true, units));
    }
    add("electrical",
        board("",
              "",
              "<Spec name=\"S\">"
              "<Dielectric type=\"DIELECTRIC_CONSTANT\"><Property value=\"4\"/></Dielectric>"
              "<Dielectric type=\"LOSS_TANGENT\"><Property value=\"0.02\"/></Dielectric>"
              "<Conductor type=\"CONDUCTIVITY\"><Property value=\"6e7\" unit=\"SIEMENS/M\"/>"
              "<Property value=\"6e7\"/></Conductor></Spec>"));
    add("spec_text",
        board(
            "",
            "",
            "<Spec name=\"S\"><General type=\"MATERIAL\">"
            "<Property text=\"FR4\"/><Property name=\"COLOR\" text=\"green\"/></General></Spec>"));
    add("courtyard",
        board(features(polygon()), "", "", "<Layer name=\"Doc\" layerFunction=\"COURTYARD\"/>"));
    add("outline",
        board(
            features(polygon()), "", "", "<Layer name=\"Doc\" layerFunction=\"BOARD_OUTLINE\"/>"));
    // Features before Profile must use the completed Profile, never partial parser state.
    add("outline_before_profile",
        board(features(polygon()) + "<Profile>" + polygon() + "</Profile>",
              "",
              "",
              "<Layer name=\"Doc\" layerFunction=\"BOARD_OUTLINE\"/>",
              false));
    add("empty_dict",
        board("",
              "<Content><DictionaryColor/>"
              "<DictionaryFillDesc>  </DictionaryFillDesc></Content>"));
    add("definitions", board(package + placed + padstack + pad(), disk + dictionaries));
    add("definitions_after_instances",
        board(placed + pad() + package + padstack, disk + dictionaries));
    add("text_attribute", board("<NonstandardAttribute name=\"TEXT\" value=\"hello\"/>"));
    for (const auto* coordinate : {"x=\"1\" y=\"0\"",
                                   "x=\"0\" y=\"1\"",
                                   "x=\"1e-9\" y=\"0\"",
                                   "x=\"bad\" y=\"0\"",
                                   "x=\"nan\" y=\"0\"",
                                   "x=\"0\" y=\"inf\"",
                                   "x=\"0\"",
                                   "x=\"\" y=\"0\""}) {
        add("datum_guard_" + std::to_string(all.size()),
            board(std::string("<Datum ") + coordinate + "/>"));
    }
    for (const auto* type : {"THERMAL_CONDUCTIVITY", "CTE", "MODULUS", "SPECIFIC_HEAT"}) {
        // Exercise both actual parent-type syntax and a mixed container with a thermal child.
        add(std::string("thermal_parent_") + type,
            board("",
                  "",
                  std::string("<Spec name=\"S\"><Dielectric type=\"") + type +
                      "\"><Property type=\"DIELECTRIC_CONSTANT\" value=\"4\"/>"
                      "<Property value=\"1\"/></Dielectric></Spec>"));
        add(std::string("thermal_mixed_") + type,
            board("",
                  "",
                  std::string("<Spec name=\"S\"><Dielectric type=\"DIELECTRIC_CONSTANT\">") +
                      "<Property type=\"DIELECTRIC_CONSTANT\" value=\"4\"/><Property type=\"" +
                      type + "\" value=\"1\"/></Dielectric></Spec>"));
    }
    add("conductor_wrong_unit",
        board("",
              "",
              "<Spec name=\"S\"><Conductor type=\"CONDUCTIVITY\">"
              "<Property unit=\"SIEMENS/M\" value=\"6e7\"/><Property unit=\"W/MK\" value=\"1\"/>"
              "</Conductor></Spec>"));
    add("spec_text_guards",
        board("",
              "",
              "<Spec name=\"S\"><General type=\"MATERIAL\">"
              "<Property text=\"FR4\"/><Property name=\"DENSITY\" value=\"25\" unit=\"MICRON\"/>"
              "<Property text=\"unnamed\"/><Property name=\"\" text=\"empty\"/>"
              "<Property name=\"PLATING_THICKNESS\" value=\"25\" unit=\"MICRON\"/>"
              "</General></Spec>"));
    add("plating_consumed",
        board("<LayerFeature layerRef=\"Drill\"><Set>"
              "<Hole name=\"V\" diameter=\"1\" x=\"2\" y=\"2\"><SpecRef id=\"S\"/></Hole>"
              "</Set></LayerFeature>",
              "",
              "<Spec name=\"S\"><General type=\"MATERIAL\">"
              "<Property name=\"PLATING_THICKNESS\" value=\"25\" unit=\"MICRON\"/>"
              "</General></Spec>",
              "<Layer name=\"Drill\" layerFunction=\"DRILL\">"
              "<Span fromLayer=\"Cu\" toLayer=\"Cu\"/></Layer>"));
    add("outline_one_dbu",
        board(features(polygon("4.001")),
              "",
              "",
              "<Layer name=\"Doc\" layerFunction=\"BOARD_OUTLINE\"/>"));
    add("outline_no_profile",
        board(features(polygon()),
              "",
              "",
              "<Layer name=\"Doc\" layerFunction=\"BOARD_OUTLINE\"/>",
              false));
    add("outline_empty",
        board(features(""), "", "", "<Layer name=\"Doc\" layerFunction=\"BOARD_OUTLINE\"/>"));
    add("outline_partial_unknown",
        board(features(polygon() + "<VendorPoint x=\"8\" y=\"0\"/>"),
              "",
              "",
              "<Layer name=\"Doc\" layerFunction=\"BOARD_OUTLINE\"/>"));
    add("nonempty_dict",
        board("",
              "<Content><DictionaryColor><EntryColor id=\"C\"/>"
              "</DictionaryColor><DictionaryFillDesc><EntryFillDesc id=\"F\"/>"
              "</DictionaryFillDesc></Content>"));
    add("no_instances", board(package + padstack, disk + dictionaries));
    add("wrong_pin", board(package + placed + padstack + pad("J1", "missing"), disk));
    add("wrong_component", board(package + placed + padstack + pad("absent"), disk));
    add("wrong_package",
        board(package + "<Component refDes=\"J1\" packageRef=\"missing\"/>" + padstack + pad(),
              disk));
    add("wrong_padstack", board(padstack + pad("J1", "1", "other"), disk));
    add("unmodeled_pad",
        board(package + placed + padstack + pad("J1", "1", "PS", "absent"), disk + dictionaries));
    add("unreferenced_dictionaries", board("", dictionaries));
    add("external_user_ref",
        board(package + placed + padstack + pad() + "<UserPrimitiveRef id=\"user\"/>",
              disk + dictionaries));
    add("external_line_ref",
        board(package + placed + padstack + pad() + "<LineDescRef id=\"line\"/>",
              disk + dictionaries));
    add("mixed_padstack_refs",
        board(package + placed + padstack + pad() +
                  "<PadStackDef name=\"unused\"><PadstackPadDef><UserPrimitiveRef id=\"user\"/>"
                  "</PadstackPadDef></PadStackDef>",
              disk + dictionaries));
    add("mixed_dictionary_entries",
        board(package + placed + padstack + pad(),
              disk +
                  "<DictionaryUser><EntryUser id=\"user\"/><EntryUser "
                  "id=\"unused\"/></DictionaryUser>"
                  "<DictionaryLineDesc><EntryLineDesc id=\"line\"/><EntryLineDesc id=\"unused\"/>"
                  "</DictionaryLineDesc>"));
    add("empty_definition_dictionaries", board("", "<DictionaryUser/><DictionaryLineDesc/>"));
    add("unread_package_outline",
        board("<Package name=\"P\"><Outline>"
              "<LineDescRef id=\"line\"/></Outline></Package>" +
                  placed,
              dictionaries));
    add("ambiguous_package", board(package + package + placed + pad(), disk));
    add("ambiguous_entry",
        board(package + placed + padstack + pad(),
              disk + "<DictionaryUser><EntryUser id=\"user\"/><EntryUser "
                     "id=\"user\"/></DictionaryUser>"));
    add("wrong_paths",
        board("<PinRef/><LayerRef/><StepRef/><FunctionMode/><BomRef/><AvlRef/>"
              "<Role/><Enterprise/><Person/><FileRevision/><SoftwarePackage/><Certification/>"
              "<BomHeader/><BomItem/><RefDes/><Characteristics/><Textual/><AvlHeader/><AvlItem/>"
              "<VendorExtension/><PickupPoint/><OtherSideView/>"
              "<Set><NonstandardAttribute name=\"FOOTPRINT_COUNT\"/><Datum x=\"0\" y=\"0\"/>"
              "<Pad><PinRef/></Pad></Set>"));
    for (const auto* function : {"SILKSCREEN", "DOCUMENT", "ASSEMBLY"})
        add(std::string("document_") + function,
            board(features(polygon()),
                  "",
                  "",
                  std::string("<Layer name=\"Doc\" layerFunction=\"") + function + "\"/>"));
    add("soldermask_spr",
        board(package + placed + padstack + pad("J1", "1", "PS", "disk", "Mask"),
              disk,
              "",
              "<Layer name=\"Mask\" layerFunction=\"SOLDERMASK\"/>"
              "<Stackup><StackupGroup><StackupLayer layerOrGroupRef=\"Mask\" thickness=\"0.01\"/>"
              "</StackupGroup></Stackup>"));
    add("same_line",
        board(
            "<NonstandardAttribute name=\"FOOTPRINT_COUNT\"/>"
            "<NonstandardAttribute name=\"TEXT\"/><NonstandardAttribute name=\"FOOTPRINT_COUNT\"/>"
            "<Datum x=\"0\" y=\"0\"/><Datum x=\"1\" y=\"0\"/>"));
    const std::string outline_lines =
        "<UserSpecial><Line startX=\"0\" startY=\"0\" endX=\"4\" endY=\"0\"/>"
        "<Line startX=\"4\" startY=\"0\" endX=\"4\" endY=\"3\"/>"
        "<Line startX=\"4\" startY=\"3\" endX=\"0\" endY=\"3\"/>"
        "<Line startX=\"0\" startY=\"3\" endX=\"0\" endY=\"0\"/></UserSpecial>";
    add("outline_lines",
        board(features(outline_lines),
              "",
              "",
              "<Layer name=\"Doc\" layerFunction=\"BOARD_OUTLINE\"/>"));
    add("outline_unknown_curve",
        board(features(polygon() + "<PolyStepCurve x=\"4\" y=\"3\" centerX=\"6\" centerY=\"0\"/>"),
              "",
              "",
              "<Layer name=\"Doc\" layerFunction=\"BOARD_OUTLINE\"/>"));
    add("spec_wrong_parent",
        board("",
              "",
              "<General type=\"MATERIAL\">"
              "<Property name=\"COLOR\" text=\"red\"/></General>"
              "<Spec name=\"S\"><General type=\"OTHER\"><Property name=\"COLOR\" text=\"red\"/>"
              "</General></Spec>"));
    all.push_back({"kicad4l", "tests/integration/ecad/data/odbpp/kicad4l.ipc2581.xml", true});
    return all;
}

using Record = std::tuple<std::string, std::string, std::uint64_t>;
struct Frozen {
    std::vector<Record> raw;
    std::array<int, 4> counts;
    std::string accepted;
    std::map<std::string, std::string> promoted;
    std::string odb_sha256;
};
// Captured with the unchanged reader/classifier at the complete baseline SHA above.
// These literals include the ordered IDs, original tiers/suggestions and all 11 fields.
const std::map<std::string, Frozen> snapshots =
    {
        {"ambiguous_entry",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 9, 0},
             // BEGIN ambiguous_entry accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":9,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END ambiguous_entry accepted
             // BEGIN ambiguous_entry promotions
             {
                 {"board/PickupPoint/10", "def-instanced"},
                 {"board/OtherSideView/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10", "def-instanced"},
                 {"board/PadstackPadDef/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10/1", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
             },
             // END ambiguous_entry promotions
             "1122cecc3efe07f41eda669d0378801daab5467cdeb181fac47c12b33cffff94",
         }},
        {"ambiguous_package",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 7, 0},
             // BEGIN ambiguous_package accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":7,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END ambiguous_package accepted
             // BEGIN ambiguous_package promotions
             {
                 {"board/PinRef/10", "netlist"},
             },
             // END ambiguous_package promotions
             "8033a5a026735a91075bfabdbd15774a93fd0027b1ffce5afb3262a9b7347e59",
         }},
        {"bom",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "BomHeader", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "BomItem", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "RefDes", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Characteristics", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Textual", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Avl", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "AvlHeader", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "AvlItem", 2},
             },
             {0, 0, 8, 0},
             // BEGIN bom accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":8,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/BomHeader/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/BomItem/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/RefDes/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Characteristics/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Textual/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Avl/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/AvlHeader/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/AvlItem/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END bom accepted
             // BEGIN bom promotions
             {
                 {"board/BomHeader/2", "bom"},
                 {"board/BomItem/2", "bom"},
                 {"board/RefDes/2", "bom"},
                 {"board/Characteristics/2", "bom"},
                 {"board/Textual/2", "bom"},
                 {"board/Avl/2", "bom"},
                 {"board/AvlHeader/2", "bom"},
                 {"board/AvlItem/2", "bom"},
             },
             // END bom promotions
             "a57ea0994005a5cc0fdbc9b422ee5196122bd5fe0578d75bd364b36b1cad9c93",
         }},
        {"conductor_wrong_unit",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 3, 0},
             // BEGIN conductor_wrong_unit accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Conductor/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END conductor_wrong_unit accepted
             // BEGIN conductor_wrong_unit promotions
             {},
             // END conductor_wrong_unit promotions
             "ccf07cc0235fd45901e88e260034c54677a2bbd3c23d5dc41509ec03eba09c78",
         }},
        {"content",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StepRef", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "FunctionMode", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "BomRef", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "AvlRef", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StepRef", 2},
             },
             {0, 0, 6, 0},
             // BEGIN content accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":6,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/LayerRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/FunctionMode/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/BomRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/AvlRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StepRef/2/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END content accepted
             // BEGIN content promotions
             {
                 {"board/LayerRef/2", "content"},
                 {"board/StepRef/2", "content"},
                 {"board/FunctionMode/2", "content"},
                 {"board/BomRef/2", "content"},
                 {"board/AvlRef/2", "content"},
                 {"board/StepRef/2/1", "content"},
             },
             // END content promotions
             "27d7e1eca46f2a5343a6aec5f5275e1cf6f35319552d31a7761e874e862d9fae",
         }},
        {"courtyard",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 10},
             },
             {0, 0, 1, 0},
             // BEGIN courtyard accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END courtyard accepted
             // BEGIN courtyard promotions
             {
                 {"board/Features/10", "doc-layer"},
             },
             // END courtyard promotions
             "42fb8178c3ee3b4d80e4bb829bde736f08212238cb805150c4a5c0b4d218f07a",
         }},
        {"datum_INCH",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 1, 0},
             // BEGIN datum_INCH accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END datum_INCH accepted
             // BEGIN datum_INCH promotions
             {
                 {"board/Datum/10", "datum"},
             },
             // END datum_INCH promotions
             "3ac120181fea19d476d7b071c52f1f43a1e30a9fe6c7c79a8b85f12fbf2b9f40",
         }},
        {"datum_MICRON",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 1, 0},
             // BEGIN datum_MICRON accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END datum_MICRON accepted
             // BEGIN datum_MICRON promotions
             {
                 {"board/Datum/10", "datum"},
             },
             // END datum_MICRON promotions
             "3ac120181fea19d476d7b071c52f1f43a1e30a9fe6c7c79a8b85f12fbf2b9f40",
         }},
        {"datum_MILLIMETER",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 1, 0},
             // BEGIN datum_MILLIMETER accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END datum_MILLIMETER accepted
             // BEGIN datum_MILLIMETER promotions
             {
                 {"board/Datum/10", "datum"},
             },
             // END datum_MILLIMETER promotions
             "3ac120181fea19d476d7b071c52f1f43a1e30a9fe6c7c79a8b85f12fbf2b9f40",
         }},
        {"datum_guard_17",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 1, 0},
             // BEGIN datum_guard_17 accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END datum_guard_17 accepted
             // BEGIN datum_guard_17 promotions
             {},
             // END datum_guard_17 promotions
             "3ac120181fea19d476d7b071c52f1f43a1e30a9fe6c7c79a8b85f12fbf2b9f40",
         }},
        {"datum_guard_18",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 1, 0},
             // BEGIN datum_guard_18 accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END datum_guard_18 accepted
             // BEGIN datum_guard_18 promotions
             {},
             // END datum_guard_18 promotions
             "3ac120181fea19d476d7b071c52f1f43a1e30a9fe6c7c79a8b85f12fbf2b9f40",
         }},
        {"datum_guard_19",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 1, 0},
             // BEGIN datum_guard_19 accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END datum_guard_19 accepted
             // BEGIN datum_guard_19 promotions
             {},
             // END datum_guard_19 promotions
             "3ac120181fea19d476d7b071c52f1f43a1e30a9fe6c7c79a8b85f12fbf2b9f40",
         }},
        {"datum_guard_20",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 1, 0},
             // BEGIN datum_guard_20 accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END datum_guard_20 accepted
             // BEGIN datum_guard_20 promotions
             {},
             // END datum_guard_20 promotions
             "3ac120181fea19d476d7b071c52f1f43a1e30a9fe6c7c79a8b85f12fbf2b9f40",
         }},
        {"datum_guard_21",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 1, 0},
             // BEGIN datum_guard_21 accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END datum_guard_21 accepted
             // BEGIN datum_guard_21 promotions
             {},
             // END datum_guard_21 promotions
             "3ac120181fea19d476d7b071c52f1f43a1e30a9fe6c7c79a8b85f12fbf2b9f40",
         }},
        {"datum_guard_22",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 1, 0},
             // BEGIN datum_guard_22 accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END datum_guard_22 accepted
             // BEGIN datum_guard_22 promotions
             {},
             // END datum_guard_22 promotions
             "3ac120181fea19d476d7b071c52f1f43a1e30a9fe6c7c79a8b85f12fbf2b9f40",
         }},
        {"datum_guard_23",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 1, 0},
             // BEGIN datum_guard_23 accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END datum_guard_23 accepted
             // BEGIN datum_guard_23 promotions
             {},
             // END datum_guard_23 promotions
             "3ac120181fea19d476d7b071c52f1f43a1e30a9fe6c7c79a8b85f12fbf2b9f40",
         }},
        {"datum_guard_24",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 1, 0},
             // BEGIN datum_guard_24 accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END datum_guard_24 accepted
             // BEGIN datum_guard_24 promotions
             {},
             // END datum_guard_24 promotions
             "3ac120181fea19d476d7b071c52f1f43a1e30a9fe6c7c79a8b85f12fbf2b9f40",
         }},
        {"definitions",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 10, 0},
             // BEGIN definitions accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":10,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END definitions accepted
             // BEGIN definitions promotions
             {
                 {"board/DictionaryUser/2", "def-instanced"},
                 {"board/EntryUser/2", "def-instanced"},
                 {"board/DictionaryLineDesc/2", "def-instanced"},
                 {"board/EntryLineDesc/2", "def-instanced"},
                 {"board/PickupPoint/10", "def-instanced"},
                 {"board/OtherSideView/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10", "def-instanced"},
                 {"board/PadstackPadDef/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10/1", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
             },
             // END definitions promotions
             "d187d1ea6ee7f47ec107a826771e76676a9f9af742c9fca22aca574ff82c95d4",
         }},
        {"definitions_after_instances",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
             },
             {0, 0, 10, 0},
             // BEGIN definitions_after_instances accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":10,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END definitions_after_instances accepted
             // BEGIN definitions_after_instances promotions
             {
                 {"board/DictionaryUser/2", "def-instanced"},
                 {"board/EntryUser/2", "def-instanced"},
                 {"board/DictionaryLineDesc/2", "def-instanced"},
                 {"board/EntryLineDesc/2", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
                 {"board/PickupPoint/10", "def-instanced"},
                 {"board/OtherSideView/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10", "def-instanced"},
                 {"board/PadstackPadDef/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10/1", "def-instanced"},
             },
             // END definitions_after_instances promotions
             "b7bd7050ee02843478581c3cd9a42e6364de38c4604c9a50092da1cf72879ae8",
         }},
        {"document_ASSEMBLY",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 10},
             },
             {0, 0, 1, 0},
             // BEGIN document_ASSEMBLY accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END document_ASSEMBLY accepted
             // BEGIN document_ASSEMBLY promotions
             {},
             // END document_ASSEMBLY promotions
             "42fb8178c3ee3b4d80e4bb829bde736f08212238cb805150c4a5c0b4d218f07a",
         }},
        {"document_DOCUMENT",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 10},
             },
             {0, 0, 1, 0},
             // BEGIN document_DOCUMENT accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END document_DOCUMENT accepted
             // BEGIN document_DOCUMENT promotions
             {},
             // END document_DOCUMENT promotions
             "42fb8178c3ee3b4d80e4bb829bde736f08212238cb805150c4a5c0b4d218f07a",
         }},
        {"document_SILKSCREEN",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 10},
             },
             {0, 0, 1, 0},
             // BEGIN document_SILKSCREEN accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END document_SILKSCREEN accepted
             // BEGIN document_SILKSCREEN promotions
             {},
             // END document_SILKSCREEN promotions
             "42fb8178c3ee3b4d80e4bb829bde736f08212238cb805150c4a5c0b4d218f07a",
         }},
        {"electrical",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 7, 0},
             // BEGIN electrical accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":7,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Dielectric/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END electrical accepted
             // BEGIN electrical promotions
             {
                 {"board/Dielectric/3", "spec-electrical"},
                 {"board/Property/3", "spec-electrical"},
                 {"board/Dielectric/3/1", "spec-electrical"},
                 {"board/Property/3/1", "spec-electrical"},
                 {"board/Conductor/3", "spec-electrical"},
                 {"board/Property/3/2", "spec-electrical"},
                 {"board/Property/3/3", "spec-electrical"},
             },
             // END electrical promotions
             "55b79a72897133842f33b6475da6280f9ff959ff655c89f8511602ccb97bb46b",
         }},
        {"empty_definition_dictionaries",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 2},
             },
             {0, 0, 2, 0},
             // BEGIN empty_definition_dictionaries accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END empty_definition_dictionaries accepted
             // BEGIN empty_definition_dictionaries promotions
             {},
             // END empty_definition_dictionaries promotions
             "e5b715480659b5ab7dc258a7d8c4beaa42dc21db0706bb17e6b5483fbbb92fc7",
         }},
        {"empty_dict",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryColor", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryFillDesc", 2},
             },
             {0, 0, 2, 0},
             // BEGIN empty_dict accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryColor/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryFillDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END empty_dict accepted
             // BEGIN empty_dict promotions
             {
                 {"board/DictionaryColor/2", "empty-dict"},
                 {"board/DictionaryFillDesc/2", "empty-dict"},
             },
             // END empty_dict promotions
             "c1b3454d9455b565ee9687a62b9339eebb0062c65fefd209ac3a34b6930a3cd6",
         }},
        {"external_line_ref",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 10, 0},
             // BEGIN external_line_ref accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":10,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END external_line_ref accepted
             // BEGIN external_line_ref promotions
             {
                 {"board/PickupPoint/10", "def-instanced"},
                 {"board/OtherSideView/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10", "def-instanced"},
                 {"board/PadstackPadDef/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10/1", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
                 {"board/DictionaryUser/2", "def-instanced"},
                 {"board/EntryUser/2", "def-instanced"},
             },
             // END external_line_ref promotions
             "d187d1ea6ee7f47ec107a826771e76676a9f9af742c9fca22aca574ff82c95d4",
         }},
        {"external_user_ref",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "UserPrimitiveRef", 10},
             },
             {0, 0, 10, 1},
             // BEGIN external_user_ref accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":10,"C":1},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/UserPrimitiveRef/10","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]})REPORT",
             // END external_user_ref accepted
             // BEGIN external_user_ref promotions
             {
                 {"board/PickupPoint/10", "def-instanced"},
                 {"board/OtherSideView/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10", "def-instanced"},
                 {"board/PadstackPadDef/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10/1", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
                 {"board/DictionaryLineDesc/2", "def-instanced"},
                 {"board/EntryLineDesc/2", "def-instanced"},
             },
             // END external_user_ref promotions
             "87bdeedacd095d8c21b697fa1c7a2f853bcd55e615888316f8a1c096905c8199",
         }},
        {"header",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Role", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Enterprise", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Person", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "FileRevision", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "SoftwarePackage", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Certification", 2},
             },
             {0, 0, 6, 0},
             // BEGIN header accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":6,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Role/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Enterprise/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Person/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/FileRevision/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/SoftwarePackage/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Certification/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END header accepted
             // BEGIN header promotions
             {
                 {"board/Role/2", "header"},
                 {"board/Enterprise/2", "header"},
                 {"board/Person/2", "header"},
                 {"board/FileRevision/2", "header"},
                 {"board/SoftwarePackage/2", "header"},
                 {"board/Certification/2", "header"},
             },
             // END header promotions
             "048e75b965cdd6a36769c182e8ff209e84ff27809c7c4ffac7ef1f3010bd4639",
         }},
        {"kicad4l",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "SkippedLayer:F.Silkscreen:SILKSCREEN", 0},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "SkippedLayer:F.Paste:SOLDERPASTE", 0},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "SolderMask:F.Mask", 0},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "SolderMask:B.Mask", 0},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "SkippedLayer:B.Paste:SOLDERPASTE", 0},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "SkippedLayer:B.Silkscreen:SILKSCREEN", 0},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "FunctionMode", 4},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StepRef", 5},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 6},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 7},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 8},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 9},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 11},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 12},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 13},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 14},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 15},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 16},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 17},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 18},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "BomRef", 19},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "AvlRef", 20},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryColor", 21},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 22},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 23},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 26},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryFillDesc", 30},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 31},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 32},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Oval", 33},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 35},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Oval", 36},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 39},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 40},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 48},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Role", 59},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Enterprise", 60},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Person", 61},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "FileRevision", 64},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "SoftwarePackage", 65},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Certification", 66},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "BomHeader", 71},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StepRef", 72},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "BomItem", 74},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "RefDes", 75},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Characteristics", 76},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Textual", 77},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 86},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 87},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 92},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 98},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 99},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 101},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 102},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 109},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 110},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 116},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 118},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 119},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 121},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 122},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 129},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 130},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 136},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 138},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 139},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 141},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 142},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 149},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 150},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 156},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 158},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 159},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 161},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 162},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 169},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 170},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 176},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 177},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 179},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 180},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 185},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 191},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 192},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "NonstandardAttribute", 276},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 278},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 282},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 286},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 290},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 297},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 301},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 305},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 309},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 314},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 355},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 358},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 362},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 591},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 592},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 596},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 597},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 605},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 606},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 610},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 611},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 835},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 836},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 840},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 841},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 849},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 850},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 854},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 855},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 1079},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 1080},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 1084},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 1085},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "NonstandardAttribute", 1091},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 1092},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Polyline", 1095},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Polyline", 1107},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 1339},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 1340},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 1344},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 1345},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 1351},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "SlotCavity", 1376},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Xform", 1378},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Oval", 1379},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Avl", 1386},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "AvlHeader", 1387},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "AvlItem", 1388},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "ViaPlatingMissing:F.Cu:B.Cu:1", 0},
             },
             {0, 0, 127, 3},
             // BEGIN kicad4l accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":127,"C":3},"b_tier_accepted":null,"defects":[
{"defect_id":"board/SolderMask%3AF.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"solder mask not modeled","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/SolderMask%3AB.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"solder mask not modeled","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/FunctionMode/4","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StepRef/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/6","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/7","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/8","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/11","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/12","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/13","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/14","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/15","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/16","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/17","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/18","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/BomRef/19","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/AvlRef/20","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryColor/21","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/22","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/23","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/26","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryFillDesc/30","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryStandard/31","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryStandard/32","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Oval/33","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},
{"defect_id":"board/EntryStandard/35","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Oval/36","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},
{"defect_id":"board/DictionaryUser/39","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/40","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/48","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Role/59","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Enterprise/60","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Person/61","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/FileRevision/64","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/SoftwarePackage/65","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Certification/66","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/BomHeader/71","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StepRef/72","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/BomItem/74","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/RefDes/75","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Characteristics/76","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Textual/77","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/86","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/87","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/92","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/98","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/99","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/101","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/102","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/109","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/110","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/116","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/118","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/119","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/121","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/122","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/129","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/130","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/136","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/138","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/139","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/141","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/142","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/149","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/150","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/156","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/158","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/159","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/161","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/162","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/169","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/170","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/176","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/177","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/179","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/180","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/185","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/191","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/192","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/NonstandardAttribute/276","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/278","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/282","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/286","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/290","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/297","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/301","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/305","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/309","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Datum/314","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/355","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/358","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/362","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/591","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/592","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/596","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/597","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/605","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/606","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/610","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/611","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/835","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/836","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/840","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/841","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/849","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/850","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/854","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/855","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/1079","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/1080","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/1084","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/1085","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/NonstandardAttribute/1091","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Features/1092","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Polyline/1095","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Polyline/1107","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/1339","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/1340","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/1344","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/1345","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Features/1351","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/SlotCavity/1376","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Xform/1378","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Oval/1379","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},
{"defect_id":"board/Avl/1386","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/AvlHeader/1387","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/AvlItem/1388","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/via_plating_missing/F.Cu-B.Cu","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm missing via plating","action":"confirm missing via plating","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END kicad4l accepted
             // BEGIN kicad4l promotions
             {
                 {"board/PinRef/592", "netlist"},
                 {"board/PinRef/597", "netlist"},
                 {"board/PinRef/606", "netlist"},
                 {"board/PinRef/611", "netlist"},
                 {"board/PinRef/836", "netlist"},
                 {"board/PinRef/841", "netlist"},
                 {"board/PinRef/850", "netlist"},
                 {"board/PinRef/855", "netlist"},
                 {"board/PinRef/1080", "netlist"},
                 {"board/PinRef/1085", "netlist"},
                 {"board/PinRef/1340", "netlist"},
                 {"board/PinRef/1345", "netlist"},
                 {"board/FunctionMode/4", "content"},
                 {"board/LayerRef/6", "content"},
                 {"board/LayerRef/7", "content"},
                 {"board/LayerRef/8", "content"},
                 {"board/LayerRef/9", "content"},
                 {"board/LayerRef/10", "content"},
                 {"board/LayerRef/11", "content"},
                 {"board/LayerRef/12", "content"},
                 {"board/LayerRef/13", "content"},
                 {"board/LayerRef/14", "content"},
                 {"board/LayerRef/15", "content"},
                 {"board/LayerRef/16", "content"},
                 {"board/LayerRef/17", "content"},
                 {"board/LayerRef/18", "content"},
                 {"board/BomRef/19", "content"},
                 {"board/AvlRef/20", "content"},
                 {"board/StepRef/5", "content"},
                 {"board/BomHeader/71", "bom"},
                 {"board/BomItem/74", "bom"},
                 {"board/RefDes/75", "bom"},
                 {"board/Characteristics/76", "bom"},
                 {"board/Textual/77", "bom"},
                 {"board/Avl/1386", "bom"},
                 {"board/AvlHeader/1387", "bom"},
                 {"board/AvlItem/1388", "bom"},
                 {"board/Role/59", "header"},
                 {"board/Enterprise/60", "header"},
                 {"board/Person/61", "header"},
                 {"board/FileRevision/64", "header"},
                 {"board/SoftwarePackage/65", "header"},
                 {"board/Certification/66", "header"},
                 {"board/NonstandardAttribute/276", "step-attr"},
                 {"board/Datum/314", "datum"},
                 {"board/Dielectric/101", "spec-electrical"},
                 {"board/Conductor/109", "spec-electrical"},
                 {"board/Dielectric/118", "spec-electrical"},
                 {"board/Dielectric/121", "spec-electrical"},
                 {"board/Conductor/129", "spec-electrical"},
                 {"board/Dielectric/138", "spec-electrical"},
                 {"board/Dielectric/141", "spec-electrical"},
                 {"board/Conductor/149", "spec-electrical"},
                 {"board/Dielectric/158", "spec-electrical"},
                 {"board/Dielectric/161", "spec-electrical"},
                 {"board/Conductor/169", "spec-electrical"},
                 {"board/Dielectric/179", "spec-electrical"},
                 {"board/Property/102", "spec-electrical"},
                 {"board/Property/110", "spec-electrical"},
                 {"board/Property/119", "spec-electrical"},
                 {"board/Property/122", "spec-electrical"},
                 {"board/Property/130", "spec-electrical"},
                 {"board/Property/139", "spec-electrical"},
                 {"board/Property/142", "spec-electrical"},
                 {"board/Property/150", "spec-electrical"},
                 {"board/Property/159", "spec-electrical"},
                 {"board/Property/162", "spec-electrical"},
                 {"board/Property/170", "spec-electrical"},
                 {"board/Property/180", "spec-electrical"},
                 {"board/DictionaryColor/21", "empty-dict"},
                 {"board/DictionaryFillDesc/30", "empty-dict"},
                 {"board/Features/1351", "outline-doc"},
                 {"board/EntryLineDesc/23", "def-instanced"},
                 {"board/PickupPoint/355", "def-instanced"},
             },
             // END kicad4l promotions
             "a15331713ee9ea661600c9f11ac322423478892700d6f70b19c2b52087927fc2",
         }},
        {"mixed_dictionary_entries",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 12, 0},
             // BEGIN mixed_dictionary_entries accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":12,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/2/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END mixed_dictionary_entries accepted
             // BEGIN mixed_dictionary_entries promotions
             {
                 {"board/PickupPoint/10", "def-instanced"},
                 {"board/OtherSideView/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10", "def-instanced"},
                 {"board/PadstackPadDef/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10/1", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
                 {"board/EntryUser/2", "def-instanced"},
                 {"board/EntryLineDesc/2", "def-instanced"},
             },
             // END mixed_dictionary_entries promotions
             "1bdebc60853a990e1c54a619d96f6ac9c800587fa23b4605d08f460543a6a12f",
         }},
        {"mixed_padstack_refs",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
             },
             {0, 0, 11, 0},
             // BEGIN mixed_padstack_refs accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":11,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END mixed_padstack_refs accepted
             // BEGIN mixed_padstack_refs promotions
             {
                 {"board/PickupPoint/10", "def-instanced"},
                 {"board/OtherSideView/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10", "def-instanced"},
                 {"board/PadstackPadDef/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10/1", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
                 {"board/DictionaryLineDesc/2", "def-instanced"},
                 {"board/EntryLineDesc/2", "def-instanced"},
             },
             // END mixed_padstack_refs promotions
             "d78eb98d80a0ba4b8628457c11cc15b91790587ba0970dcbd77005bb9bc37a7c",
         }},
        {"netlist",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 1, 0},
             // BEGIN netlist accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END netlist accepted
             // BEGIN netlist promotions
             {
                 {"board/PinRef/10", "netlist"},
             },
             // END netlist promotions
             "cb90f7c6e6598c995797bc034292fb5864f0ce9f2e8e7318eb87034ccc831fd3",
         }},
        {"no_instances",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
             },
             {0, 0, 9, 0},
             // BEGIN no_instances accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":9,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END no_instances accepted
             // BEGIN no_instances promotions
             {
                 {"board/DictionaryLineDesc/2", "def-instanced"},
                 {"board/EntryLineDesc/2", "def-instanced"},
             },
             // END no_instances promotions
             "aafc753311e5e17ea2bc5f0a8c312f69b9df3ce37f7bb065c8c48b9b668ba44d",
         }},
        {"nonempty_dict",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryColor", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryColor", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryFillDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryFillDesc", 2},
             },
             {0, 0, 4, 0},
             // BEGIN nonempty_dict accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":4,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryColor/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryColor/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryFillDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryFillDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END nonempty_dict accepted
             // BEGIN nonempty_dict promotions
             {},
             // END nonempty_dict promotions
             "7dce86de83f1994f30af6ccbf146d499d9fec94d5fa9d9d8e59d8c035d50a242",
         }},
        {"outline",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 10},
             },
             {0, 0, 1, 0},
             // BEGIN outline accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END outline accepted
             // BEGIN outline promotions
             {
                 {"board/Features/10", "outline-doc"},
             },
             // END outline promotions
             "42fb8178c3ee3b4d80e4bb829bde736f08212238cb805150c4a5c0b4d218f07a",
         }},
        {"outline_before_profile",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 9},
             },
             {0, 0, 1, 0},
             // BEGIN outline_before_profile accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END outline_before_profile accepted
             // BEGIN outline_before_profile promotions
             {
                 {"board/Features/9", "outline-doc"},
             },
             // END outline_before_profile promotions
             "16937df46664d7e0a2b5d6bce27bba60d26ac9c0dcc7352b44b9e56d1c1efd6c",
         }},
        {"outline_empty",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 10},
             },
             {0, 0, 1, 0},
             // BEGIN outline_empty accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END outline_empty accepted
             // BEGIN outline_empty promotions
             {},
             // END outline_empty promotions
             "42fb8178c3ee3b4d80e4bb829bde736f08212238cb805150c4a5c0b4d218f07a",
         }},
        {"outline_lines",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 10},
             },
             {0, 0, 1, 0},
             // BEGIN outline_lines accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END outline_lines accepted
             // BEGIN outline_lines promotions
             {
                 {"board/Features/10", "outline-doc"},
             },
             // END outline_lines promotions
             "42fb8178c3ee3b4d80e4bb829bde736f08212238cb805150c4a5c0b4d218f07a",
         }},
        {"outline_no_profile",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 9},
             },
             {0, 0, 1, 0},
             // BEGIN outline_no_profile accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END outline_no_profile accepted
             // BEGIN outline_no_profile promotions
             {},
             // END outline_no_profile promotions
             "16937df46664d7e0a2b5d6bce27bba60d26ac9c0dcc7352b44b9e56d1c1efd6c",
         }},
        {"outline_one_dbu",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 10},
             },
             {0, 0, 1, 0},
             // BEGIN outline_one_dbu accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END outline_one_dbu accepted
             // BEGIN outline_one_dbu promotions
             {},
             // END outline_one_dbu promotions
             "42fb8178c3ee3b4d80e4bb829bde736f08212238cb805150c4a5c0b4d218f07a",
         }},
        {"outline_partial_unknown",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "VendorPoint", 10},
             },
             {0, 0, 2, 0},
             // BEGIN outline_partial_unknown accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/VendorPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END outline_partial_unknown accepted
             // BEGIN outline_partial_unknown promotions
             {},
             // END outline_partial_unknown promotions
             "8fde5b63d81eec143432da0a49ddc0a4d7e45a1753b1f52ff63905e158828c55",
         }},
        {"outline_unknown_curve",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PolyStepCurve", 10},
             },
             {0, 0, 1, 1},
             // BEGIN outline_unknown_curve accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":1},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Features/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PolyStepCurve/10","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]})REPORT",
             // END outline_unknown_curve accepted
             // BEGIN outline_unknown_curve promotions
             {},
             // END outline_unknown_curve promotions
             "361f18c5c7c52ca89c46e8c87f075da0eaceaa3ddbbaf9391725efcff3625d98",
         }},
        {"plating_consumed",
         {
             {},
             {0, 0, 0, 0},
             // BEGIN plating_consumed accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":0},"b_tier_accepted":null,"defects":[
]})REPORT",
             // END plating_consumed accepted
             // BEGIN plating_consumed promotions
             {},
             // END plating_consumed promotions
             "79c904a339c910a3205bae12c88286eebc21b0cfc388abc45c9af4eb983decd3",
         }},
        {"same_line",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "NonstandardAttribute", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "NonstandardAttribute", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "NonstandardAttribute", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
             },
             {0, 0, 5, 0},
             // BEGIN same_line accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":5,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/NonstandardAttribute/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/NonstandardAttribute/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/NonstandardAttribute/10/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Datum/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END same_line accepted
             // BEGIN same_line promotions
             {
                 {"board/NonstandardAttribute/10", "step-attr"},
                 {"board/NonstandardAttribute/10/2", "step-attr"},
                 {"board/Datum/10", "datum"},
             },
             // END same_line promotions
             "6bbbfdd7504a6e3a380265e69fffc0c27ea7f41317aad976117b87fb959433d6",
         }},
        {"soldermask_spr",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "SolderMask:Mask", 0},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 8, 0},
             // BEGIN soldermask_spr accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":8,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/SolderMask%3AMask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"solder mask not modeled","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END soldermask_spr accepted
             // BEGIN soldermask_spr promotions
             {
                 {"board/PickupPoint/10", "def-instanced"},
                 {"board/OtherSideView/10", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
             },
             // END soldermask_spr promotions
             "b4f46167950a5ad6c9df157000b79b97d40e300bfd7d5d18ded12940624994f2",
         }},
        {"spec_text",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 1, 0},
             // BEGIN spec_text accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END spec_text accepted
             // BEGIN spec_text promotions
             {
                 {"board/Property/3", "spec-text"},
             },
             // END spec_text promotions
             "fe3524300ed29e435fd18cb84f459920da70013182f4c12a9746a348b44816b5",
         }},
        {"spec_text_guards",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 3, 0},
             // BEGIN spec_text_guards accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END spec_text_guards accepted
             // BEGIN spec_text_guards promotions
             {},
             // END spec_text_guards promotions
             "37bb85dc77d4f3bea85f816ec2f7d88e295f0459a5aa9e3ff88bfb72839a76df",
         }},
        {"spec_wrong_parent",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 2, 0},
             // BEGIN spec_wrong_parent accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END spec_wrong_parent accepted
             // BEGIN spec_wrong_parent promotions
             {},
             // END spec_wrong_parent promotions
             "3066144c88d42203ab9bbeea400afbc9f9cda3597d604cddd6638531cfe102a3",
         }},
        {"step_attr",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "NonstandardAttribute", 10},
             },
             {0, 0, 1, 0},
             // BEGIN step_attr accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/NonstandardAttribute/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END step_attr accepted
             // BEGIN step_attr promotions
             {
                 {"board/NonstandardAttribute/10", "step-attr"},
             },
             // END step_attr promotions
             "6e53ff8ec6148859ad1cd8dae09818a6fe1604dd01dd818a128ad6f13ef83767",
         }},
        {"text_attribute",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "NonstandardAttribute", 10},
             },
             {0, 0, 1, 0},
             // BEGIN text_attribute accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/NonstandardAttribute/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END text_attribute accepted
             // BEGIN text_attribute promotions
             {},
             // END text_attribute promotions
             "6e53ff8ec6148859ad1cd8dae09818a6fe1604dd01dd818a128ad6f13ef83767",
         }},
        {"thermal_mixed_CTE",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 3, 0},
             // BEGIN thermal_mixed_CTE accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Dielectric/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END thermal_mixed_CTE accepted
             // BEGIN thermal_mixed_CTE promotions
             {},
             // END thermal_mixed_CTE promotions
             "4093cbf16229b38b813448d380ab85b2eee2272d63e6f425e6f42ac08bbc5528",
         }},
        {"thermal_mixed_MODULUS",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 3, 0},
             // BEGIN thermal_mixed_MODULUS accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Dielectric/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END thermal_mixed_MODULUS accepted
             // BEGIN thermal_mixed_MODULUS promotions
             {},
             // END thermal_mixed_MODULUS promotions
             "4093cbf16229b38b813448d380ab85b2eee2272d63e6f425e6f42ac08bbc5528",
         }},
        {"thermal_mixed_SPECIFIC_HEAT",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 3, 0},
             // BEGIN thermal_mixed_SPECIFIC_HEAT accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Dielectric/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END thermal_mixed_SPECIFIC_HEAT accepted
             // BEGIN thermal_mixed_SPECIFIC_HEAT promotions
             {},
             // END thermal_mixed_SPECIFIC_HEAT promotions
             "4093cbf16229b38b813448d380ab85b2eee2272d63e6f425e6f42ac08bbc5528",
         }},
        {"thermal_mixed_THERMAL_CONDUCTIVITY",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 3, 0},
             // BEGIN thermal_mixed_THERMAL_CONDUCTIVITY accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Dielectric/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END thermal_mixed_THERMAL_CONDUCTIVITY accepted
             // BEGIN thermal_mixed_THERMAL_CONDUCTIVITY promotions
             {},
             // END thermal_mixed_THERMAL_CONDUCTIVITY promotions
             "4093cbf16229b38b813448d380ab85b2eee2272d63e6f425e6f42ac08bbc5528",
         }},
        {"thermal_parent_CTE",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 3, 0},
             // BEGIN thermal_parent_CTE accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Dielectric/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END thermal_parent_CTE accepted
             // BEGIN thermal_parent_CTE promotions
             {},
             // END thermal_parent_CTE promotions
             "4093cbf16229b38b813448d380ab85b2eee2272d63e6f425e6f42ac08bbc5528",
         }},
        {"thermal_parent_MODULUS",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 3, 0},
             // BEGIN thermal_parent_MODULUS accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Dielectric/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END thermal_parent_MODULUS accepted
             // BEGIN thermal_parent_MODULUS promotions
             {},
             // END thermal_parent_MODULUS promotions
             "4093cbf16229b38b813448d380ab85b2eee2272d63e6f425e6f42ac08bbc5528",
         }},
        {"thermal_parent_SPECIFIC_HEAT",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 3, 0},
             // BEGIN thermal_parent_SPECIFIC_HEAT accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Dielectric/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END thermal_parent_SPECIFIC_HEAT accepted
             // BEGIN thermal_parent_SPECIFIC_HEAT promotions
             {},
             // END thermal_parent_SPECIFIC_HEAT promotions
             "4093cbf16229b38b813448d380ab85b2eee2272d63e6f425e6f42ac08bbc5528",
         }},
        {"thermal_parent_THERMAL_CONDUCTIVITY",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
             },
             {0, 0, 3, 0},
             // BEGIN thermal_parent_THERMAL_CONDUCTIVITY accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Dielectric/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END thermal_parent_THERMAL_CONDUCTIVITY accepted
             // BEGIN thermal_parent_THERMAL_CONDUCTIVITY promotions
             {},
             // END thermal_parent_THERMAL_CONDUCTIVITY promotions
             "4093cbf16229b38b813448d380ab85b2eee2272d63e6f425e6f42ac08bbc5528",
         }},
        {"unmodeled_pad",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 11, 0},
             // BEGIN unmodeled_pad accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":11,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END unmodeled_pad accepted
             // BEGIN unmodeled_pad promotions
             {
                 {"board/PickupPoint/10", "def-instanced"},
                 {"board/OtherSideView/10", "def-instanced"},
                 {"board/DictionaryLineDesc/2", "def-instanced"},
                 {"board/EntryLineDesc/2", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
             },
             // END unmodeled_pad promotions
             "1e134490b7863d89868c81d745b0db4bad95c8157b527b1ea09ac9d4259d40c7",
         }},
        {"unread_package_outline",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 2},
             },
             {0, 0, 4, 0},
             // BEGIN unread_package_outline accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":4,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END unread_package_outline accepted
             // BEGIN unread_package_outline promotions
             {},
             // END unread_package_outline promotions
             "f2bbf770fd1f6dcce3f3cacbb87dca83d775414d46b5458a3dc02ddaf6cac20c",
         }},
        {"unreferenced_dictionaries",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 2},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "EntryLineDesc", 2},
             },
             {0, 0, 4, 0},
             // BEGIN unreferenced_dictionaries accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":4,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END unreferenced_dictionaries accepted
             // BEGIN unreferenced_dictionaries promotions
             {},
             // END unreferenced_dictionaries promotions
             "f2bbf770fd1f6dcce3f3cacbb87dca83d775414d46b5458a3dc02ddaf6cac20c",
         }},
        {"wrong_component",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 6, 0},
             // BEGIN wrong_component accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":6,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END wrong_component accepted
             // BEGIN wrong_component promotions
             {
                 {"board/PickupPoint/10", "def-instanced"},
                 {"board/OtherSideView/10", "def-instanced"},
                 {"board/PadstackPadDef/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10/1", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
             },
             // END wrong_component promotions
             "1cc817170fec873032ea9ded3519c00a6a7f3927b8e65d8921bb0ac33c1ad3bd",
         }},
        {"wrong_package",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PackageRef:missing", 0},
             },
             {0, 0, 7, 0},
             // BEGIN wrong_package accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":7,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PackageRef%3Amissing/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END wrong_package accepted
             // BEGIN wrong_package promotions
             {
                 {"board/PadstackPadDef/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10/1", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
             },
             // END wrong_package promotions
             "bbc77002d5e794f3a72b534b58d522f5a431879d2898449f9ab04fd08775796c",
         }},
        {"wrong_padstack",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 3, 0},
             // BEGIN wrong_padstack accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END wrong_padstack accepted
             // BEGIN wrong_padstack promotions
             {
                 {"board/PinRef/10", "netlist"},
             },
             // END wrong_padstack promotions
             "c4c6b55b72913ad6e9b26210f8d1f25fca0c14ba1d82acd92bf0cf5e8430a10c",
         }},
        {"wrong_paths",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StepRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "FunctionMode", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "BomRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "AvlRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Role", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Enterprise", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Person", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "FileRevision", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "SoftwarePackage", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Certification", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "BomHeader", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "BomItem", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "RefDes", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Characteristics", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Textual", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "AvlHeader", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "AvlItem", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "VendorExtension", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "NonstandardAttribute", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "Pad", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 25, 1},
             // BEGIN wrong_paths accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":25,"C":1},"b_tier_accepted":null,"defects":[
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StepRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/FunctionMode/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/BomRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/AvlRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Role/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Enterprise/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Person/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/FileRevision/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/SoftwarePackage/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Certification/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/BomHeader/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/BomItem/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/RefDes/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Characteristics/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Textual/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/AvlHeader/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/AvlItem/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/VendorExtension/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/NonstandardAttribute/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Datum/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Pad/10","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},
{"defect_id":"board/PinRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END wrong_paths accepted
             // BEGIN wrong_paths promotions
             {},
             // END wrong_paths promotions
             "8035c5d3dd175e9e920a6a3e002267e25e2d1b64999d66a17155d503b68d747c",
         }},
        {"wrong_pin",
         {
             {
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PickupPoint", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "OtherSideView", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
                 {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 10},
             },
             {0, 0, 6, 0},
             // BEGIN wrong_pin accepted
             R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":6,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/PickupPoint/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/OtherSideView/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]})REPORT",
             // END wrong_pin accepted
             // BEGIN wrong_pin promotions
             {
                 {"board/PickupPoint/10", "def-instanced"},
                 {"board/OtherSideView/10", "def-instanced"},
                 {"board/PadstackPadDef/10", "def-instanced"},
                 {"board/StandardPrimitiveRef/10/1", "def-instanced"},
                 {"board/PinRef/10", "netlist"},
             },
             // END wrong_pin promotions
             "1cc817170fec873032ea9ded3519c00a6a7f3927b8e65d8921bb0ac33c1ad3bd",
         }},
};
const std::map<std::string, int> fixture_promotions = {
    {"Avl", 1},
    {"AvlHeader", 1},
    {"AvlItem", 1},
    {"AvlRef", 1},
    {"BomHeader", 1},
    {"BomItem", 1},
    {"BomRef", 1},
    {"Certification", 1},
    {"Characteristics", 1},
    {"Conductor", 4},
    {"Datum", 1},
    {"DictionaryColor", 1},
    {"DictionaryFillDesc", 1},
    {"Dielectric", 8},
    {"Enterprise", 1},
    {"EntryLineDesc", 1},
    {"Features", 1},
    {"FileRevision", 1},
    {"FunctionMode", 1},
    {"LayerRef", 13},
    {"NonstandardAttribute", 1},
    {"Person", 1},
    {"PickupPoint", 1},
    {"PinRef", 12},
    {"Property", 12},
    {"RefDes", 1},
    {"Role", 1},
    {"SoftwarePackage", 1},
    {"StepRef", 1},
    {"Textual", 1},
};
const std::map<std::string, std::string> fixture_geometry = {
    {"stack.json", "a573c6f4f343a6ff73c2ced0fe46ea828430e8b60e99299a0ecafc909b959de8"},
    {"board.polyset", "d1e56d360fc1a4d2b7054bd1303fc2c8c71f2d8a58bb47001e2e6ed60e51c4b5"},
};
const std::map<std::string, std::string> definition_geometry = {
    {"stack.json", "049aa7bac38f3161cb28d419dce4a8653247d07a86bbcace1977ab6b0cd3db4b"},
    {"board.polyset", "e22082e4d3775aac3e97c01a16552f5948bcd20b154f6a67189e01b0814696aa"},
};
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}
// Retain serialized objects byte for byte, including all eleven frozen I3 fields.
std::vector<std::string> objects(const std::string& report) {
    const auto marker = report.find("\"defects\":[");
    if (marker == std::string::npos)
        throw std::runtime_error("missing defects array");
    std::vector<std::string> out;
    unsigned depth = 0;
    std::size_t begin = 0;
    bool quoted = false, escaped = false;
    for (std::size_t i = marker + 11; i < report.size(); ++i) {
        const char c = report[i];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"') {
            quoted = true;
        } else if (c == '{') {
            if (depth++ == 0)
                begin = i;
        } else if (c == '}') {
            if (!depth)
                throw std::runtime_error("unbalanced JSON object");
            if (--depth == 0)
                out.push_back(report.substr(begin, i - begin + 1));
        } else if (c == ']' && !depth) {
            return out;
        }
    }
    throw std::runtime_error("unterminated defects array");
}
std::string field(const std::string& object, const std::string& key) {
    const std::string marker = "\"" + key + "\":\"";
    const auto begin = object.find(marker);
    if (begin == std::string::npos)
        throw std::runtime_error("missing report field " + key);
    const auto value = begin + marker.size();
    return object.substr(value, object.find('"', value) - value);
}
void replace_field(std::string& object, const std::string& key, const std::string& value) {
    const auto old = field(object, key);
    const auto start = object.find("\"" + key + "\":\"") + key.size() + 4;
    object.replace(start, old.size(), value);
}
std::string counts_json(const std::array<int, 4>& counts) {
    return "\"tier_counts\":{\"Weld\":" + std::to_string(counts[0]) +
           ",\"A\":" + std::to_string(counts[1]) + ",\"B\":" + std::to_string(counts[2]) +
           ",\"C\":" + std::to_string(counts[3]) + "}";
}
void verify(const Case& item, const Frozen& frozen) {
    const Temporary tmp;
    const auto input = item.fixture ? root / item.xml : tmp.write("board.xml", item.xml);
    const auto result = ecad::read_ipc2581(input);
    if (!result)
        throw std::runtime_error("read_ipc2581: " + result.error);
    const auto& model = result.model;
    std::vector<Record> actual_raw;
    for (const auto& row : model.report.unsupported)
        actual_raw.emplace_back(row.code, row.element, row.line);
    check(actual_raw == frozen.raw,
          item.name + ": ordered raw (code, element, line) matches baseline");
    std::set<std::string> visited;
    std::map<std::string, int> changed_by_element;
    const auto before = objects(frozen.accepted);
    const auto classified = chipcae::importer::ecad_defects::board_defects(model.report, false);
    std::array<int, 4> measured{};
    for (const auto& defect : classified)
        ++measured[static_cast<std::size_t>(defect.tier)];
    int changed = 0;
    for (const bool accept : {true, false}) {
        const auto policy = geometry::apply_repair_policy(classified, {}, accept);
        std::string error;
        if (!geometry::write_import_report(tmp.path, {}, policy.defects, &error))
            throw std::runtime_error(error);
        const auto serialized = bytes(tmp.path / "import_report.json");
        const auto after = objects(serialized);
        check(after.size() == before.size(), item.name + ": no defect is removed or added");
        const auto size = std::min(after.size(), before.size());
        for (std::size_t i = 0; i < size; ++i) {
            const auto id = field(before[i], "defect_id");
            const auto label = item.name + ": " + id + (accept ? " accepted" : " unaccepted");
            check(field(after[i], "defect_id") == id, label + ": original ID and occurrence order");
            auto expected = before[i];
            const auto promotion = frozen.promoted.find(id);
            if (promotion != frozen.promoted.end()) {
                const auto action =
                    promotion->second == "def-instanced" ? definition_action : metadata_action;
                check(field(before[i], "tier") == "B",
                      label + ": baseline promotion must start at B");
                replace_field(expected, "tier", "A");
                replace_field(expected, "suggestion", action);
                replace_field(expected, "action", action);
                check(field(after[i], "tier") == "A",
                      label + ": expected tier A for " + promotion->second + ", got " +
                          field(after[i], "tier"));
                check(field(after[i], "suggestion") == action, label + ": exact A suggestion");
                check(field(after[i], "action") == action, label + ": exact repair-policy action");
                if (accept) {
                    visited.insert(id);
                    if (field(after[i], "tier") == "A") {
                        ++changed;
                        const auto element_end = id.find('/', 6);
                        ++changed_by_element[id.substr(6, element_end - 6)];
                    }
                }
            } else if (field(before[i], "tier") == "B" &&
                       field(before[i], "suggestion") == confirm) {
                check(field(after[i], "tier") == "B" && field(after[i], "suggestion") == confirm,
                      label + ": guard/context miss must retain exact B suggestion");
            }
            // In rejected mode an unchanged B carries the established confirmation reason.
            // The three-field-only comparison is made above in accepted-B mode.
            if (!accept && field(expected, "tier") == "B") {
                replace_field(expected, "action", "");
                replace_field(
                    expected, "rejection_reason", "tier B defect requires explicit confirmation");
            }
            check(after[i] == expected,
                  label + ": exact eleven serialized fields; only authorized fields change");
        }
        auto expected_counts = frozen.counts;
        const auto promoted = static_cast<int>(frozen.promoted.size());
        expected_counts[1] += promoted;
        expected_counts[2] -= promoted;
        check(serialized.find(counts_json(expected_counts)) != std::string::npos,
              item.name + ": all four serialized tier_counts");
    }
    check(visited.size() == frozen.promoted.size(),
          item.name + ": every explicit promotion ID exists");
    check(changed == static_cast<int>(frozen.promoted.size()),
          item.name + ": actual reclassification count");
    check(measured[1] - frozen.counts[1] == changed && frozen.counts[2] - measured[2] == changed &&
              measured[0] == frozen.counts[0] && measured[3] == frozen.counts[3],
          item.name + ": measured delta A = -delta B; Weld and C unchanged");
    if (item.fixture) {
        check(changed_by_element == fixture_promotions, "kicad4l: measured promotions by element");
        for (const auto& [element, count] : changed_by_element)
            std::cout << "RECLASSIFIED " << element << ' ' << count << '\n';
    }
    // Once the reader fills context, this exercises those same entries with odb=true.
    // The test intentionally compiles against the three-field baseline aggregate too.
    const auto odb = geometry::apply_repair_policy(
        chipcae::importer::ecad_defects::board_defects(model.report, true), {}, true);
    std::string error;
    if (!geometry::write_import_report(tmp.path, {}, odb.defects, &error))
        throw std::runtime_error(error);
    check(chipcae::mesh::sha256(bytes(tmp.path / "import_report.json")) == frozen.odb_sha256,
          item.name + ": odb=true retains every baseline byte even for future annotated entries");
    if (item.name == "plating_consumed") {
        check(model.report.unsupported.empty(),
              "PLATING_THICKNESS remains consumed, never promoted");
        check(model.vias.size() == 1, "plating Property continues to create a via");
        if (!model.vias.empty()) {
            check(std::abs(model.vias[0].outer_radius_dbu - 500) <= 1e-10 &&
                      std::abs(model.vias[0].inner_radius_dbu - 475) <= 1e-10,
                  "25 MICRON plating preserves outer/inner radii (absolute tolerance 1e-10 dbu)");
        }
    }
    if (item.name == "definitions" || item.name == "definitions_after_instances") {
        check(model.layers.size() == 1 && model.layers[0].polygons.size() == 1,
              item.name + ": reference-positive input really has a modeled copper Pad");
        check(model.components.size() == 1 && model.components[0].pads.size() == 1 &&
                  model.components[0].outline.points.size() == 4,
              item.name + ": placed package pin and outline were read");
    }
    if (item.fixture || item.name == "definitions") {
        auto mapped = model;
        if (!ecad::apply_material_map(
                mapped, root / "tests/integration/import_cli/material_map.json", &error) ||
            !ecad::to_stack(mapped, tmp.path / "stack.json", tmp.path / "board.polyset", &error))
            throw std::runtime_error(error);
        const auto& hashes = item.fixture ? fixture_geometry : definition_geometry;
        for (const auto& [file, digest] : hashes)
            check(chipcae::mesh::sha256(bytes(tmp.path / file)) == digest,
                  item.name + ": " + file + " is byte-identical to baseline");
    }
}
// Legacy aggregate callers default to fail-closed metadata behavior. Geometry C and
// SolderMask/ViaPlatingMissing suggestions must survive the new classification branch.
void verify_legacy_classifier() {
    ecad::ImportReport report;
    report.unsupported = {{"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 7},
                          {"IMPORT_UNSUPPORTED_ELEMENT", "Circle", 7},
                          {"IMPORT_UNSUPPORTED_ELEMENT", "SolderMask:Mask", 0},
                          {"IMPORT_UNSUPPORTED_ELEMENT", "ViaPlatingMissing:Cu:Cu:2", 0}};
    const auto defects =
        geometry::apply_repair_policy(
            chipcae::importer::ecad_defects::board_defects(report, false), {}, true)
            .defects;
    check(defects.size() == 4, "legacy aggregate: exact defect count");
    if (defects.size() != 4)
        return;
    check(defects[0].tier == geometry::Tier::B && defects[0].suggestion == confirm,
          "empty context keeps bare PinRef B");
    check(defects[1].tier == geometry::Tier::C, "geometry C remains C");
    check(defects[2].suggestion == "solder mask not modeled", "SolderMask special suggestion");
    check(defects[3].tier == geometry::Tier::B &&
              defects[3].suggestion == "confirm missing via plating" &&
              defects[3].defect_id == "board/via_plating_missing/Cu-Cu",
          "ViaPlatingMissing tier, suggestion and stable span ID");
}
} // namespace
int main() {
    std::cout << "Frozen baseline " << baseline_sha << '\n';
    for (const auto& item : cases()) {
        const auto before = failures;
        try {
            verify(item, snapshots.at(item.name));
        } catch (const std::exception& error) {
            check(false, item.name + ": " + error.what());
        }
        std::cout << (failures == before ? "PASS " : "FAIL ") << item.name << '\n';
    }
    verify_legacy_classifier();
    std::cout << "Assertion failures: " << failures << '\n';
    return failures ? 1 : 0;
}
