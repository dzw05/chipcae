#define CHIPCAE_ENABLE_ECAD 1
#include "../../../kernel/src/import_ecad_defects.hpp"
#include "../../../schema/src/sha256.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
namespace ecad = chipcae::geometry::ecad;
namespace geometry = chipcae::geometry;
using Record = std::tuple<std::string, std::string, std::uint64_t>;
struct Deletion {
    std::size_t raw_index;
    std::string defect_id;
};
struct Layer {
    std::string name;
    double thickness;
    std::vector<double> polygon_areas;
};
struct Snapshot {
    std::string name, input;
    bool fixture;
    std::vector<Record> raw;
    std::vector<Deletion> deleted;
    std::vector<Layer> layers;
    std::array<int, 4> tier_counts;
    std::string rejected, accepted;
};
const std::filesystem::path root = CHIPCAE_SOURCE_DIR;
struct Temporary {
    std::filesystem::path path;
    Temporary() {
        std::random_device random;
        do {
            path = std::filesystem::temp_directory_path() /
                   ("report-consumed-" + std::to_string(random()) + "-" + std::to_string(random()));
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
int failures = 0;
void check(bool value, const std::string& message) {
    if (!value) {
        ++failures;
        std::cerr << "FAIL " << message << '\n';
    }
}
bool close(double actual, double expected) {
    // Absolute 1e-10 plus relative 5.5e-6, as required by the brief.
    return std::isfinite(actual) &&
           std::abs(actual - expected) <= 1e-10 + 5.5e-6 * std::abs(expected);
}
double area(const ecad::Polygon& polygon, double dbu_m) {
    long double twice = 0;
    for (std::size_t i = 0; i < polygon.points.size(); ++i) {
        const auto a = polygon.points[i], b = polygon.points[(i + 1) % polygon.points.size()];
        twice += static_cast<long double>(a[0]) * b[1] - static_cast<long double>(b[0]) * a[1];
    }
    return std::abs(static_cast<double>(twice)) * dbu_m * dbu_m / 2;
}
// Split objects emitted by the existing report writer, retaining every serialized byte.
// The frozen baselines below use one object per line solely for reviewability.
std::vector<std::string> defect_objects(const std::string& report) {
    const auto marker = report.find("\"defects\":[");
    if (marker == std::string::npos)
        throw std::runtime_error("missing defects array");
    std::vector<std::string> objects;
    std::size_t begin = 0;
    unsigned depth = 0;
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
            if (depth == 0)
                throw std::runtime_error("unbalanced report object");
            if (--depth == 0)
                objects.push_back(report.substr(begin, i - begin + 1));
        } else if (c == ']' && depth == 0) {
            return objects;
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
std::string counts_json(const std::array<int, 4>& counts) {
    return "\"tier_counts\":{\"Weld\":" + std::to_string(counts[0]) +
           ",\"A\":" + std::to_string(counts[1]) + ",\"B\":" + std::to_string(counts[2]) +
           ",\"C\":" + std::to_string(counts[3]) + "}";
}
void verify_report(const Snapshot& baseline,
                   const ecad::EcadModel& model,
                   const Temporary& tmp,
                   bool accept) {
    const auto& frozen = accept ? baseline.accepted : baseline.rejected;
    const auto before = defect_objects(frozen);
    auto counts = baseline.tier_counts;
    std::set<std::string> deleted;
    for (const auto& deletion : baseline.deleted) {
        const auto found = std::find_if(before.begin(), before.end(), [&](const auto& object) {
            return field(object, "defect_id") == deletion.defect_id;
        });
        if (found == before.end())
            throw std::runtime_error("baseline deletion ID missing: " + deletion.defect_id);
        const auto tier = field(*found, "tier");
        const std::array<std::string, 4> tiers{"Weld", "A", "B", "C"};
        const auto position = std::find(tiers.begin(), tiers.end(), tier);
        if (position == tiers.end())
            throw std::runtime_error("unknown baseline tier");
        --counts[static_cast<std::size_t>(position - tiers.begin())];
        deleted.insert(deletion.defect_id);
    }
    std::vector<std::string> expected;
    for (const auto& object : before)
        if (!deleted.contains(field(object, "defect_id")))
            expected.push_back(object);
    auto defects = chipcae::importer::ecad_defects::board_defects(model.report, false);
    const auto policy = geometry::apply_repair_policy(std::move(defects), {}, accept);
    std::string error;
    if (!geometry::write_import_report(tmp.path, {}, policy.defects, &error))
        throw std::runtime_error(error);
    const auto actual = bytes(tmp.path / "import_report.json");
    check(defect_objects(actual) == expected,
          baseline.name + ": ordered serialized defects (all 11 fields, including size_unit), " +
              (accept ? "accepted" : "unaccepted"));
    check(actual.find(counts_json(counts)) != std::string::npos,
          baseline.name + ": all tier_counts keys equal baseline minus explicit deletions");
    for (const auto& object : defect_objects(actual))
        check(!deleted.contains(field(object, "defect_id")),
              baseline.name + ": consumed ID remains: " + field(object, "defect_id"));
}

// All snapshots were captured with read_ipc2581() before changing the reader.
// HEAD = origin/main = adcbe340aa73b06daccc271809e8d3cf26c33eef (includes #294).
// Only the explicit raw indices / defect IDs below are removed from expectations.
// Public fixtures are read without options, mapping or solder-mask restoration.

const std::vector<Deletion> modeled_deletions = {
    {0, "board/DictionaryStandard/5"},
    {1, "board/EntryStandard/6"},
    {2, "board/StandardPrimitiveRef/9"},
};
const Snapshot modeled{
    "modeled",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER"/>
<CadData>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/></StackupGroup></Stackup>
<DictionaryStandard>
<EntryStandard id="disk"><Circle diameter="1"/></EntryStandard>
</DictionaryStandard>
<Step name="acceptance"><LayerFeature layerRef="F.Cu"><Set>
<Pad padstackDefRef="artwork"><Location x="20" y="20"/><StandardPrimitiveRef id="disk"/></Pad>
</Set></LayerFeature></Step>
</CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 6},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 9},
    },
    modeled_deletions,
    {
        {"F.Cu", 3.5000000000000004e-05, {7.8552399999999996e-07}},
    },
    {0, 0, 3, 0},
    // BEGIN modeled rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/6","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]})REPORT",
    // END modeled rejected
    ""};

const std::vector<Deletion> contexts_deletions = {
    {1, "board/DictionaryStandard/5"},
    {2, "board/EntryStandard/6"},
    {8, "board/StandardPrimitiveRef/15"},
    {13, "board/DictionaryStandard/21"},
    {14, "board/EntryStandard/21"},
};
const Snapshot contexts{
    "contexts",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER"/>
<CadData>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/></StackupGroup></Stackup>
<DictionaryStandard>
<EntryStandard id="disk"><Circle diameter="1"/></EntryStandard>
</DictionaryStandard>
<StandardPrimitiveRef id="disk"/>
<Package name="P"><Pin number="1"><StandardPrimitiveRef id="disk"/></Pin></Package>
<PadStackDef name="P"><PadstackPadDef layerRef="F.Cu"><StandardPrimitiveRef id="disk"/></PadstackPadDef></PadStackDef>
<Layer name="F.Mask" layerFunction="SOLDERMASK"/>
<StackupLayer layerOrGroupRef="F.Mask" thickness="0.01"/>
<LayerFeature layerRef="F.Mask"><Set><Pad padstackDefRef="artwork"><Location x="20" y="20"/><StandardPrimitiveRef id="disk"/></Pad></Set></LayerFeature>
<Step name="acceptance"><LayerFeature layerRef="F.Cu"><Set>
<Pad padstackDefRef="artwork"><Location x="20" y="20"/><StandardPrimitiveRef id="disk"/></Pad>
<StandardPrimitiveRef id="disk"/>
<Pad padstackDefRef="artwork"><StandardPrimitiveRef id="later"/></Pad>
<Pad padstackDefRef="artwork"><StandardPrimitiveRef id="missing"/></Pad>
<Pad padstackDefRef="artwork"><StandardPrimitiveRef id="DISK"/></Pad>
</Set></LayerFeature></Step>
<DictionaryStandard><EntryStandard id="later"><Circle diameter="1"/></EntryStandard></DictionaryStandard>
</CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "SolderMask:F.Mask", 0},
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 6},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 8},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 9},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 10},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 13},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 15},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 16},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 17},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 18},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 19},
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 21},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 21},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Circle", 16},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Circle", 17},
    },
    contexts_deletions,
    {
        {"F.Cu", 3.5000000000000004e-05, {7.8552399999999996e-07}},
    },
    {0, 0, 15, 2},
    // BEGIN contexts rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":15,"C":2},"b_tier_accepted":null,"defects":[
{"defect_id":"board/SolderMask%3AF.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/DictionaryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/6","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/8","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/13","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/15","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/16","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/17","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/18","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/19","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/DictionaryStandard/21","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/21","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Circle/16","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},
{"defect_id":"board/Circle/17","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]})REPORT",
    // END contexts rejected
    ""};

const std::vector<Deletion> empty_dictionary_deletions = {};
const Snapshot empty_dictionary{
    "empty_dictionary",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER"/>
<CadData>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/></StackupGroup></Stackup>
<DictionaryStandard/>
</CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 5},
    },
    empty_dictionary_deletions,
    {
        {"F.Cu", 3.5000000000000004e-05, {}},
    },
    {0, 0, 1, 0},
    // BEGIN empty_dictionary rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]})REPORT",
    // END empty_dictionary rejected
    ""};

const std::vector<Deletion> oval_dictionary_deletions = {};
const Snapshot oval_dictionary{
    "oval_dictionary",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER"/>
<CadData>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/></StackupGroup></Stackup>
<DictionaryStandard><EntryStandard id="oval"><Oval width="2" height="1"/></EntryStandard></DictionaryStandard>
</CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Oval", 5},
    },
    oval_dictionary_deletions,
    {
        {"F.Cu", 3.5000000000000004e-05, {}},
    },
    {0, 0, 2, 1},
    // BEGIN oval_dictionary rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":1},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Oval/5","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]})REPORT",
    // END oval_dictionary rejected
    ""};

const std::vector<Deletion> empty_id_dictionary_deletions = {};
const Snapshot empty_id_dictionary{
    "empty_id_dictionary",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER"/>
<CadData>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/></StackupGroup></Stackup>
<DictionaryStandard><EntryStandard><Circle diameter="1"/></EntryStandard></DictionaryStandard>
</CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Circle", 5},
    },
    empty_id_dictionary_deletions,
    {
        {"F.Cu", 3.5000000000000004e-05, {}},
    },
    {0, 0, 2, 1},
    // BEGIN empty_id_dictionary rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":1},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Circle/5","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]})REPORT",
    // END empty_id_dictionary rejected
    ""};

const std::vector<Deletion> mixed_dictionary_deletions = {
    {1, "board/EntryStandard/6"},
};
const Snapshot mixed_dictionary{
    "mixed_dictionary",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER"/>
<CadData>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/></StackupGroup></Stackup>
<DictionaryStandard>
<EntryStandard id="disk"><Circle diameter="1"/></EntryStandard>
<EntryStandard id="oval"><Oval width="2" height="1"/></EntryStandard></DictionaryStandard>
</CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 6},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 7},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Oval", 7},
    },
    mixed_dictionary_deletions,
    {
        {"F.Cu", 3.5000000000000004e-05, {}},
    },
    {0, 0, 3, 1},
    // BEGIN mixed_dictionary rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":1},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/6","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/7","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Oval/7","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]})REPORT",
    // END mixed_dictionary rejected
    ""};

const std::vector<Deletion> circle_dictionary_deletions = {
    {0, "board/DictionaryStandard/5"},
    {1, "board/EntryStandard/5"},
    {3, "board/EntryStandard/5/1"},
};
const Snapshot circle_dictionary{
    "circle_dictionary",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER"/>
<CadData>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/></StackupGroup></Stackup>
<DictionaryStandard><EntryStandard id="disk"><Circle diameter="1"/><UnknownChild/></EntryStandard><EntryStandard id="disk2"><Circle diameter="2"/></EntryStandard></DictionaryStandard>
</CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "UnknownChild", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 5},
    },
    circle_dictionary_deletions,
    {
        {"F.Cu", 3.5000000000000004e-05, {}},
    },
    {0, 0, 4, 0},
    // BEGIN circle_dictionary rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":4,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/UnknownChild/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/5/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]})REPORT",
    // END circle_dictionary rejected
    ""};

const std::vector<Deletion> properties_deletions = {
    {1, "board/Property/4"},
};
const Snapshot properties{
    "properties",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER">
<Spec name="mat"><General type="MATERIAL">
<Property/>
<Property text="FR4"/>
<Property text="later"/>
<Property text=""/>
</General><Dielectric><Property text="dk"/></Dielectric><Conductor><Property text="conductivity"/></Conductor></Spec>
<Spec name="plating"><General type="MATERIAL"><Property name="PLATING_THICKNESS" value="20" unit="MICRON"/></General></Spec>
<Spec name="both"><General type="MATERIAL"><Property name="PLATING_THICKNESS" text="copper" value="20" unit="MICRON"/><Property text="later"/></General></Spec>
</CadHeader><CadData><Layer name="Core" layerFunction="DIELCORE"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Core" thickness="1"><SpecRef id="mat"/></StackupLayer></StackupGroup></Stackup>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/><Layer name="B.Cu" layerFunction="CONDUCTOR"/><StackupLayer layerOrGroupRef="B.Cu" thickness="0.035"/><Layer name="drill" layerFunction="DRILL"><Span fromLayer="F.Cu" toLayer="B.Cu"/></Layer><Step><LayerFeature layerRef="drill"><Hole name="plating-only" diameter="0.5"><SpecRef id="plating"/></Hole><Hole name="material-and-plating" diameter="0.5"><SpecRef id="both"/></Hole></LayerFeature></Step></CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 3},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 4},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 6},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 7},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 7},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 7},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 7},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 9},
    },
    properties_deletions,
    {
        {"Core", 0.001, {}},
        {"F.Cu", 3.5000000000000004e-05, {}},
        {"B.Cu", 3.5000000000000004e-05, {}},
    },
    {0, 0, 9, 0},
    // BEGIN properties rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":9,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/4","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/6","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/7","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/7","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Conductor/7","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/7/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]})REPORT",
    // END properties rejected
    ""};

const std::vector<Deletion> features_deletions = {
    {4, "board/Features/11"},
    {5, "board/Features/11/1"},
    {6, "board/Features/12"},
    {7, "board/Features/12/1"},
};
const Snapshot features{
    "features",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER"/>
<CadData>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/></StackupGroup></Stackup>
<Layer name="Core" layerFunction="DIELCORE"/><StackupLayer layerOrGroupRef="Core" thickness="1"/>
<Layer name="Court" layerFunction="COURTYARD"/><StackupLayer layerOrGroupRef="Court"/>
<Layer name="Outline" layerFunction="BOARD_OUTLINE"/><StackupLayer layerOrGroupRef="Outline"/>
<Layer name="Mask" layerFunction="SOLDERMASK"/><StackupLayer layerOrGroupRef="Mask" thickness="0.01"/>
<Features/>
<Step name="acceptance">
<LayerFeature layerRef="F.Cu"><Set><Features><UserSpecial><Contour><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="10" y="0"/><PolyStepSegment x="10" y="10"/><PolyStepSegment x="0" y="10"/></Polygon></Contour></UserSpecial></Features><Features/></Set></LayerFeature>
<LayerFeature layerRef="Core"><Set><Features><UserSpecial><Contour><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="10" y="0"/><PolyStepSegment x="10" y="10"/><PolyStepSegment x="0" y="10"/></Polygon></Contour></UserSpecial></Features><Features/></Set></LayerFeature>
<LayerFeature layerRef="Court"><Set><Features><UserSpecial><Contour><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="10" y="0"/><PolyStepSegment x="10" y="10"/><PolyStepSegment x="0" y="10"/></Polygon></Contour></UserSpecial></Features><Features/></Set></LayerFeature>
<LayerFeature layerRef="Outline"><Set><Features><UserSpecial><Contour><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="10" y="0"/><PolyStepSegment x="10" y="10"/><PolyStepSegment x="0" y="10"/></Polygon></Contour></UserSpecial></Features><Features/></Set></LayerFeature>
<LayerFeature layerRef="Mask"><Set><Features><UserSpecial><Contour><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="10" y="0"/><PolyStepSegment x="10" y="10"/><PolyStepSegment x="0" y="10"/></Polygon></Contour></UserSpecial></Features><Features/></Set></LayerFeature>
<LayerFeature layerRef="missing"><Set><Features><UserSpecial><Contour><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="10" y="0"/><PolyStepSegment x="10" y="10"/><PolyStepSegment x="0" y="10"/></Polygon></Contour></UserSpecial></Features><Features/></Set></LayerFeature>
</Step>
</CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "SkippedLayer:Court:COURTYARD", 0},
        {"IMPORT_UNSUPPORTED_ELEMENT", "SkippedLayer:Outline:BOARD_OUTLINE", 0},
        {"IMPORT_UNSUPPORTED_ELEMENT", "SolderMask:Mask", 0},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 9},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 11},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 11},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 12},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 12},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 13},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 13},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 14},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 14},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 15},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 15},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 16},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 16},
    },
    features_deletions,
    {
        {"F.Cu", 3.5000000000000004e-05, {9.9999999999999991e-05}},
        {"Core", 0.001, {9.9999999999999991e-05}},
    },
    {0, 0, 14, 0},
    // BEGIN features rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":14,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/SolderMask%3AMask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/11","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/11/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/12","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/12/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/13","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/13/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/14","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/14/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/15","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/15/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/16","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/16/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]})REPORT",
    // END features rejected
    ""};

const std::vector<Deletion> interleaved_deletions = {
    {0, "board/DictionaryStandard/5"},
    {1, "board/EntryStandard/6"},
    {2, "board/StandardPrimitiveRef/10"},
    {5, "board/StandardPrimitiveRef/14"},
};
const Snapshot interleaved{
    "interleaved",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER"/>
<CadData>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/></StackupGroup></Stackup>
<DictionaryStandard>
<EntryStandard id="disk"><Circle diameter="1"/></EntryStandard>
</DictionaryStandard>
<Step name="acceptance"><LayerFeature layerRef="F.Cu"><Set>
<Line startX="0" startY="0" endX="10" endY="0"><LineDesc lineWidth="0.2" lineEnd="ROUND"/></Line>
<Pad padstackDefRef="artwork"><Location x="20" y="20"/><StandardPrimitiveRef id="disk"/></Pad>
<Line startX="0" startY="5" endX="10" endY="5"/>
<StandardPrimitiveRef id="missing"/>
<Line startX="0" startY="0" endX="10" endY="0"><LineDesc lineWidth="0.2" lineEnd="ROUND"/></Line>
<Pad padstackDefRef="artwork"><Location x="20" y="20"/><StandardPrimitiveRef id="disk"/></Pad>
<StandardPrimitiveRef id="disk"/>
</Set></LayerFeature></Step>
</CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 6},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 10},
        {"IMPORT_UNSUPPORTED_ELEMENT", "LineWidthMissing:F.Cu", 11},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 12},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 14},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 15},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Circle", 15},
    },
    interleaved_deletions,
    {
        {"F.Cu", 3.5000000000000004e-05, {7.8552399999999996e-07, 2.0313229999999999e-06}},
    },
    {0, 0, 6, 2},
    // BEGIN interleaved rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":6,"C":2},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/6","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LineWidthMissing%3AF.Cu/11","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/12","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/14","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/15","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Circle/15","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]})REPORT",
    // END interleaved rejected
    ""};

const std::vector<Deletion> same_line_deletions = {
    {0, "board/DictionaryStandard/5"},
    {1, "board/EntryStandard/6"},
    {2, "board/StandardPrimitiveRef/9"},
};
const Snapshot same_line{
    "same_line",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER"/>
<CadData>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/></StackupGroup></Stackup>
<DictionaryStandard>
<EntryStandard id="disk"><Circle diameter="1"/></EntryStandard>
</DictionaryStandard>
<Step name="acceptance"><LayerFeature layerRef="F.Cu"><Set>
<Pad padstackDefRef="artwork"><Location x="20" y="20"/><StandardPrimitiveRef id="disk"/></Pad><StandardPrimitiveRef id="missing"/><StandardPrimitiveRef id="DISK"/>
</Set></LayerFeature></Step>
</CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 6},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 9},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 9},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 9},
    },
    same_line_deletions,
    {
        {"F.Cu", 3.5000000000000004e-05, {7.8552399999999996e-07}},
    },
    {0, 0, 5, 0},
    // BEGIN same_line rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":5,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/6","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/9/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/9/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]})REPORT",
    // END same_line rejected
    ""};

const std::vector<Deletion> same_line_unconsumed_deletions = {
    {0, "board/DictionaryStandard/5"},
    {1, "board/EntryStandard/6"},
};
const Snapshot same_line_unconsumed{
    "same_line_unconsumed",
    R"XML(<IPC-2581><CadHeader units="MILLIMETER"/>
<CadData>
<Layer name="F.Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="F.Cu" thickness="0.035"/></StackupGroup></Stackup>
<DictionaryStandard>
<EntryStandard id="disk"><Circle diameter="1"/></EntryStandard>
</DictionaryStandard>
<Step name="acceptance"><LayerFeature layerRef="F.Cu"><Set>
<StandardPrimitiveRef id="missing"/><StandardPrimitiveRef id="DISK"/>
</Set></LayerFeature></Step>
</CadData></IPC-2581>
)XML",
    false,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 6},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 9},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 9},
    },
    same_line_unconsumed_deletions,
    {
        {"F.Cu", 3.5000000000000004e-05, {}},
    },
    {0, 0, 4, 0},
    // BEGIN same_line_unconsumed rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":4,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/DictionaryStandard/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/6","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/9/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]})REPORT",
    // END same_line_unconsumed rejected
    ""};

const std::vector<Deletion> kicad4l_deletions = {
    {48, "board/Property/85"},
    {52, "board/Property/97"},
    {57, "board/Property/107"},
    {60, "board/Property/115"},
    {66, "board/Property/127"},
    {69, "board/Property/135"},
    {75, "board/Property/147"},
    {78, "board/Property/155"},
    {84, "board/Property/167"},
    {87, "board/Property/175"},
    {93, "board/Property/190"},
    {109, "board/Features/370"},
    {118, "board/Features/617"},
    {127, "board/Features/861"},
    {136, "board/Features/1121"},
};
const Snapshot kicad4l{
    "kicad4l",
    "tests/integration/ecad/data/odbpp/kicad4l.ipc2581.xml",
    true,
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
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 85},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 86},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 87},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 92},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 97},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 98},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 99},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 101},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 102},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 107},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 109},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 110},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 115},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 116},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 118},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 119},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 121},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 122},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 127},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 129},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 130},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 135},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 136},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 138},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 139},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 141},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 142},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 147},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 149},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 150},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 155},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 156},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 158},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 159},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 161},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 162},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 167},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 169},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 170},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 175},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 176},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 177},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 179},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 180},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 185},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 190},
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
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 370},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 591},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 592},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 596},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 597},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 605},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 606},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 610},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 611},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 617},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 835},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 836},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 840},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 841},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 849},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 850},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 854},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 855},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 861},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 1079},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 1080},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 1084},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PinRef", 1085},
        {"IMPORT_UNSUPPORTED_ELEMENT", "NonstandardAttribute", 1091},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 1092},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Polyline", 1095},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Polyline", 1107},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 1121},
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
    kicad4l_deletions,
    {
        {"F.Cu",
         3.5000000000000004e-05,
         {0.00093598986199999983,
          3.6000000000000001e-05,
          1.1845509999999999e-05,
          1.2092299999999999e-05}},
        {"DIELECTRIC_1", 0.00020000000000000001, {}},
        {"In1.Cu", 3.5000000000000004e-05, {0.000876052052}},
        {"DIELECTRIC_2", 0.0010600000000000002, {}},
        {"In2.Cu", 3.5000000000000004e-05, {0.000876052052}},
        {"DIELECTRIC_3", 0.00020000000000000001, {}},
        {"B.Cu", 3.5000000000000004e-05, {0.000876052052}},
    },
    {0, 0, 142, 3},
    // BEGIN kicad4l rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":142,"C":3},"b_tier_accepted":null,"defects":[
{"defect_id":"board/SolderMask%3AF.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/SolderMask%3AB.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/FunctionMode/4","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StepRef/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/6","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/7","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/8","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/11","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/12","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/13","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/14","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/15","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/16","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/17","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/18","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/BomRef/19","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/AvlRef/20","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/DictionaryColor/21","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/22","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/23","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryLineDesc/26","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/DictionaryFillDesc/30","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/DictionaryStandard/31","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/32","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Oval/33","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},
{"defect_id":"board/EntryStandard/35","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Oval/36","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},
{"defect_id":"board/DictionaryUser/39","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryUser/40","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryUser/48","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Role/59","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Enterprise/60","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Person/61","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/FileRevision/64","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/SoftwarePackage/65","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Certification/66","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/BomHeader/71","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StepRef/72","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/BomItem/74","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/RefDes/75","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Characteristics/76","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Textual/77","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/85","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/86","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/87","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/92","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/97","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/98","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/99","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/101","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/102","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/107","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Conductor/109","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/110","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/115","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/116","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/118","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/119","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/121","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/122","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/127","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Conductor/129","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/130","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/135","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/136","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/138","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/139","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/141","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/142","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/147","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Conductor/149","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/150","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/155","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/156","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/158","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/159","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/161","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/162","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/167","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Conductor/169","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/170","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/175","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/176","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/177","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/179","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/180","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/185","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/190","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/191","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/192","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/NonstandardAttribute/276","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/278","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/282","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/286","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/290","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/297","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/301","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/305","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/309","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Datum/314","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PickupPoint/355","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/358","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/362","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/370","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/591","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/592","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/596","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/597","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/605","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/606","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/610","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/611","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/617","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/835","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/836","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/840","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/841","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/849","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/850","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/854","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/855","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/861","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/1079","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/1080","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/1084","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/1085","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/NonstandardAttribute/1091","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/1092","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Polyline/1095","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Polyline/1107","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/1121","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/1339","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/1340","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/1344","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PinRef/1345","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/1351","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/SlotCavity/1376","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Xform/1378","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Oval/1379","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},
{"defect_id":"board/Avl/1386","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/AvlHeader/1387","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/AvlItem/1388","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/via_plating_missing/F.Cu-B.Cu","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm missing via plating","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]})REPORT",
    // END kicad4l rejected
    // BEGIN kicad4l accepted
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":142,"C":3},"b_tier_accepted":null,"defects":[
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
{"defect_id":"board/Property/85","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/86","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/87","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/92","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/97","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/98","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/99","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/101","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/102","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/107","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/109","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/110","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/115","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/116","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/118","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/119","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/121","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/122","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/127","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/129","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/130","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/135","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/136","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/138","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/139","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/141","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/142","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/147","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/149","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/150","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/155","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/156","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/158","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/159","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/161","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/162","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/167","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/169","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/170","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/175","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/176","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/177","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/179","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/180","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/185","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/190","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
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
{"defect_id":"board/Features/370","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/591","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/592","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/596","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/597","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/605","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/606","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/610","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/611","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Features/617","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/835","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/836","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/840","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/841","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/849","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/850","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/854","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/855","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Features/861","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/1079","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/1080","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/1084","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PinRef/1085","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/NonstandardAttribute/1091","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Features/1092","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Polyline/1095","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Polyline/1107","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Features/1121","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
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
{"defect_id":"board/via_plating_missing/F.Cu-B.Cu","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm missing via plating","action":"confirm missing via plating","rejection_reason":"","material_ambiguous":false}]})REPORT"
    // END kicad4l accepted
};

const std::vector<Deletion> kicad10_deletions = {
    {12, "board/DictionaryStandard/16"},
    {13, "board/EntryStandard/17"},
    {22, "board/Property/48"},
    {25, "board/Property/56"},
    {31, "board/Property/68"},
    {34, "board/Property/76"},
    {40, "board/Property/88"},
    {43, "board/Property/96"},
    {49, "board/Property/108"},
    {60, "board/Features/202"},
    {61, "board/StandardPrimitiveRef/223"},
    {62, "board/StandardPrimitiveRef/227"},
    {63, "board/Features/233"},
    {64, "board/StandardPrimitiveRef/251"},
    {65, "board/Features/257"},
    {66, "board/StandardPrimitiveRef/269"},
    {67, "board/StandardPrimitiveRef/273"},
    {68, "board/Features/279"},
    {69, "board/StandardPrimitiveRef/297"},
};
const Snapshot kicad10{
    "kicad10",
    "tests/integration/ecad/data/kicad_like/board_kicad10/board.xml",
    true,
    {
        {"IMPORT_UNSUPPORTED_ELEMENT", "FunctionMode", 4},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StepRef", 5},
        {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 6},
        {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 7},
        {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 8},
        {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 9},
        {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 10},
        {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 11},
        {"IMPORT_UNSUPPORTED_ELEMENT", "LayerRef", 12},
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryColor", 13},
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryLineDesc", 14},
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryFillDesc", 15},
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryStandard", 16},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryStandard", 17},
        {"IMPORT_UNSUPPORTED_ELEMENT", "DictionaryUser", 21},
        {"IMPORT_UNSUPPORTED_ELEMENT", "EntryUser", 22},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Role", 33},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Enterprise", 34},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Person", 35},
        {"IMPORT_UNSUPPORTED_ELEMENT", "FileRevision", 38},
        {"IMPORT_UNSUPPORTED_ELEMENT", "SoftwarePackage", 39},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Certification", 40},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 48},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 50},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 51},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 56},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 57},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 59},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 60},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 62},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 63},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 68},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 70},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 71},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 76},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 77},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 79},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 80},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 82},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 83},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 88},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 90},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 91},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 96},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 97},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 99},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 100},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Dielectric", 102},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 103},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 108},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Conductor", 110},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Property", 111},
        {"IMPORT_UNSUPPORTED_ELEMENT", "NonstandardAttribute", 159},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 162},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 166},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 170},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 174},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 181},
        {"IMPORT_UNSUPPORTED_ELEMENT", "PadstackPadDef", 185},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Datum", 190},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 202},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 223},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 227},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 233},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 251},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 257},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 269},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 273},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 279},
        {"IMPORT_UNSUPPORTED_ELEMENT", "StandardPrimitiveRef", 297},
        {"IMPORT_UNSUPPORTED_ELEMENT", "Features", 303},
        {"IMPORT_UNSUPPORTED_ELEMENT", "ViaPlatingMissing:F.Cu:In1.Cu:1", 0},
        {"IMPORT_UNSUPPORTED_ELEMENT", "ViaPlatingMissing:F.Cu:B.Cu:1", 0},
    },
    kicad10_deletions,
    {
        {"F.Cu",
         3.5000000000000004e-05,
         {2.0313229999999999e-06,
          4.1254369999999999e-06,
          2.0313229999999999e-06,
          7.8552399999999996e-07,
          7.8552399999999996e-07}},
        {"DIELECTRIC_1", 0.00048000000000000001, {}},
        {"In1.Cu", 3.5000000000000004e-05, {7.8552399999999996e-07, 7.8552399999999996e-07}},
        {"DIELECTRIC_2", 0.00050000000000000001, {}},
        {"In2.Cu", 3.5000000000000004e-05, {0.0014}},
        {"DIELECTRIC_3", 0.00048000000000000001, {}},
        {"B.Cu",
         3.5000000000000004e-05,
         {2.0313229999999999e-06,
          4.1254369999999999e-06,
          2.0313229999999999e-06,
          7.8552399999999996e-07}},
    },
    {0, 0, 73, 0},
    // BEGIN kicad10 rejected
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":73,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/FunctionMode/4","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StepRef/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/6","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/7","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/8","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/11","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/LayerRef/12","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/DictionaryColor/13","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/14","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/DictionaryFillDesc/15","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/DictionaryStandard/16","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryStandard/17","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/DictionaryUser/21","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/EntryUser/22","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Role/33","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Enterprise/34","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Person/35","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/FileRevision/38","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/SoftwarePackage/39","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Certification/40","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/48","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Conductor/50","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/51","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/56","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/57","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/59","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/60","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/62","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/63","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/68","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Conductor/70","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/71","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/76","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/77","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/79","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/80","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/82","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/83","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/88","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Conductor/90","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/91","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/96","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/97","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/99","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/100","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Dielectric/102","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/103","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/108","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Conductor/110","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Property/111","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/NonstandardAttribute/159","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/162","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/166","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/170","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/174","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/181","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/185","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Datum/190","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/202","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/223","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/227","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/233","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/251","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/257","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/269","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/273","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/279","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/297","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/Features/303","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/via_plating_missing/F.Cu-In1.Cu","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm missing via plating","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},
{"defect_id":"board/via_plating_missing/F.Cu-B.Cu","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm missing via plating","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]})REPORT",
    // END kicad10 rejected
    // BEGIN kicad10 accepted
    R"REPORT({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":73,"C":0},"b_tier_accepted":null,"defects":[
{"defect_id":"board/FunctionMode/4","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StepRef/5","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/6","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/7","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/8","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/10","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/11","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/LayerRef/12","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryColor/13","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryLineDesc/14","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryFillDesc/15","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryStandard/16","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryStandard/17","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/DictionaryUser/21","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/EntryUser/22","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Role/33","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Enterprise/34","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Person/35","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/FileRevision/38","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/SoftwarePackage/39","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Certification/40","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/48","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/50","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/51","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/56","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/57","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/59","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/60","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/62","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/63","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/68","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/70","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/71","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/76","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/77","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/79","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/80","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/82","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/83","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/88","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/90","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/91","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/96","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/97","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/99","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/100","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Dielectric/102","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/103","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/108","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Conductor/110","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Property/111","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/NonstandardAttribute/159","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/162","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/166","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/170","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/174","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/181","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/PadstackPadDef/185","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Datum/190","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Features/202","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/223","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/227","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Features/233","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/251","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Features/257","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/269","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/273","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Features/279","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/StandardPrimitiveRef/297","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/Features/303","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/via_plating_missing/F.Cu-In1.Cu","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm missing via plating","action":"confirm missing via plating","rejection_reason":"","material_ambiguous":false},
{"defect_id":"board/via_plating_missing/F.Cu-B.Cu","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm missing via plating","action":"confirm missing via plating","rejection_reason":"","material_ambiguous":false}]})REPORT"
    // END kicad10 accepted
};
void verify(const Snapshot& baseline) {
    const Temporary tmp;
    const auto input =
        baseline.fixture ? root / baseline.input : tmp.write("board.xml", baseline.input);
    auto result = ecad::read_ipc2581(input);
    if (!result)
        throw std::runtime_error(baseline.name + ": read_ipc2581: " + result.error);
    const auto& model = result.model;
    std::vector<Record> expected, actual;
    for (std::size_t i = 0; i < baseline.raw.size(); ++i)
        if (std::none_of(baseline.deleted.begin(),
                         baseline.deleted.end(),
                         [&](const auto& deletion) { return deletion.raw_index == i; }))
            expected.push_back(baseline.raw[i]);
    for (const auto& entry : model.report.unsupported)
        actual.emplace_back(entry.code, entry.element, entry.line);
    check(actual == expected,
          baseline.name +
              ": complete ordered (code, element, line) report must equal frozen "
              "baseline minus explicit consumed records; expected " +
              std::to_string(expected.size()) + ", got " + std::to_string(actual.size()));
    check(model.layers.size() == baseline.layers.size(), baseline.name + ": ordered layer count");
    for (std::size_t i = 0; i < model.layers.size() && i < baseline.layers.size(); ++i) {
        const auto& layer = model.layers[i];
        const auto& frozen = baseline.layers[i];
        check(layer.name == frozen.name, baseline.name + ": ordered layer name " + frozen.name);
        check(close(layer.thickness_m, frozen.thickness), baseline.name + ": layer thickness");
        check(layer.polygons.size() == frozen.polygon_areas.size(),
              baseline.name + ": polygon count on " + frozen.name);
        for (std::size_t j = 0; j < layer.polygons.size() && j < frozen.polygon_areas.size(); ++j)
            check(close(area(layer.polygons[j], model.dbu_m), frozen.polygon_areas[j]),
                  baseline.name + ": ordered polygon area on " + frozen.name);
    }
    // Include same-line survivors: the brief requires their original occurrence suffixes.
    // This deliberately exposes the frozen classifier's renumbering conflict after filtering.
    verify_report(baseline, model, tmp, false);
    if (!baseline.accepted.empty())
        verify_report(baseline, model, tmp, true);
    if (baseline.name == "modeled" || baseline.name == "contexts" ||
        baseline.name == "interleaved" || baseline.name == "same_line") {
        check(model.vias.empty(), baseline.name + ": artwork must not create drilled vias");
    }
    if (baseline.name == "modeled") {
        check(model.layers.size() == 1 && model.layers.front().copper &&
                  model.layers.front().polygons.size() == 1,
              "defined Circle Pad must produce exactly one copper layer/polygon");
        if (!model.layers.empty() && !model.layers.front().polygons.empty()) {
            const double actual_area =
                area(model.layers.front().polygons.front(), model.dbu_m) * 1e6;
            const double expected_area = std::acos(-1.0) / 4;
            // Preserve test_ipc2581.cpp's 1e-3 tolerance for discretized circles.
            check(std::abs(actual_area - expected_area) / expected_area <= 1e-3,
                  "defined Circle Pad retains analytic pi/4 mm^2 area");
        }
    }
    if (baseline.name == "properties") {
        check(!model.layers.empty() && model.layers.front().material == "FR4",
              "first nonempty General/MATERIAL text still supplies layer material");
        check(model.vias.size() == 2, "both plating-only and material/plating Properties parse");
        for (const auto& via : model.vias)
            check(close(via.outer_radius_dbu, 250) && close(via.inner_radius_dbu, 230),
                  via.id + ": PLATING_THICKNESS still sets radial copper thickness");
        // Acceptance item 4 requires this unconsumed material-name Property to remain reported.
        // The actual baseline above has no line-8 record: the existing plating branch already
        // consumes it silently. Keep that provenance intact and expose the conflicting demand.
        const Record plating_only{"IMPORT_UNSUPPORTED_ELEMENT", "Property", 8};
        check(std::find(actual.begin(), actual.end(), plating_only) != actual.end(),
              "properties: acceptance requires the plating-only Property report at line 8; "
              "the unchanged reader already omits it (baseline contract conflict)");
    }
    if (baseline.name == "kicad10") {
        const double pi = std::acos(-1.0);
        const auto round_area = [pi](double length, double width) {
            return length * width + pi * width * width / 4;
        };
        for (const auto& layer : model.layers) {
            if (layer.name != "F.Cu" && layer.name != "B.Cu")
                continue;
            double actual_area = 0;
            for (const auto& polygon : layer.polygons)
                actual_area += area(polygon, model.dbu_m) * 1e6;
            const double pads = layer.name == "F.Cu" ? 2 : 1;
            const double expected_area =
                2 * round_area(10, .2) + round_area(10, .4) + pads * pi / 4;
            check(std::abs(actual_area - expected_area) / expected_area <= 1e-3,
                  "KiCad10 " + layer.name + ": existing round_area expectation");
        }
    }
    if (baseline.fixture) {
        auto mapped = model;
        std::string error;
        if (!ecad::apply_material_map(
                mapped, root / "tests/integration/import_cli/material_map.json", &error) ||
            !ecad::to_stack(mapped, tmp.path / "stack.json", tmp.path / "board.polyset", &error))
            throw std::runtime_error(baseline.name + ": " + error);
        const bool first = baseline.name == "kicad4l";
        check(chipcae::mesh::sha256(bytes(tmp.path / "stack.json")) ==
                  (first ? "a573c6f4f343a6ff73c2ced0fe46ea828430e8b60e99299a0ecafc909b959de8"
                         : "fac500f5730339ccbac8ce7dd33c61f873c9f803bb8fe54940c6265b5261bc9a"),
              baseline.name + ": stack.json bytes retain baseline SHA-256");
        check(chipcae::mesh::sha256(bytes(tmp.path / "board.polyset")) ==
                  (first ? "d1e56d360fc1a4d2b7054bd1303fc2c8c71f2d8a58bb47001e2e6ed60e51c4b5"
                         : "f3c6ebf37498b9005a71716686b6ce8a4a7021025a63a4c309db80e59892b04f"),
              baseline.name + ": board.polyset bytes retain baseline SHA-256");
    }
}
} // namespace

int main() {
    for (const auto* baseline : {&modeled,
                                 &contexts,
                                 &empty_dictionary,
                                 &oval_dictionary,
                                 &mixed_dictionary,
                                 &circle_dictionary,
                                 &empty_id_dictionary,
                                 &properties,
                                 &features,
                                 &interleaved,
                                 &same_line,
                                 &same_line_unconsumed,
                                 &kicad4l,
                                 &kicad10}) {
        const int before = failures;
        try {
            verify(*baseline);
        } catch (const std::exception& e) {
            check(false, baseline->name + ": " + e.what());
        }
        std::cout << (before == failures ? "PASS " : "FAIL ") << baseline->name << '\n';
    }
    return failures ? 1 : 0;
}
