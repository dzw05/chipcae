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
constexpr auto baseline_sha = "ea3da025c4deb11163a78f266887b6067527bcea";
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
    bool fixture;
};
const std::vector<Case> inputs = {
    {"color",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color : Not specified"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"empty",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"empty_text",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text=""/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"whitespace",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="   "/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"unicode",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="颜色：绿 ２"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"same_line",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property/><Property/><Property text="green"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"keyword_conduct",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color CONDUCT sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"keyword_thermal",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color THERMAL sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"keyword_cte",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color CTE sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"keyword_expansion",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color EXPANSION sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"keyword_modulus",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color MODULUS sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"keyword_young",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color YOUNG sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"keyword_poisson",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color POISSON sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"keyword_heat",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color HEAT sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"keyword_density",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color DENSITY sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"keyword_thick",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color THICK sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"digit_0",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color 0 sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"digit_1",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color 1 sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"digit_2",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color 2 sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"digit_3",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color 3 sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"digit_4",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color 4 sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"digit_5",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color 5 sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"digit_6",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color 6 sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"digit_7",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color 7 sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"digit_8",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color 8 sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"digit_9",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Color 9 sample"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"thickness",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="Thickness 25um"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"child",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="green"><Child/></Property></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"empty_child",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property><Child/></Property></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"unit",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="green" unit="mm"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"value",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="green" value="x"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"unknown_attribute",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property text="green" extra="x"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"other_general",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="OTHER"><Property text="FR4"/><Property text="green"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"named_original",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property name="COLOR" text="THERMAL 25"><Child/></Property></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"named_value_unit",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property text="FR4"/><Property name="COLOR" value="1" unit="mm"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"wrong_property_ancestry",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><General type="MATERIAL"><Property text="FR4"/><Property/></General></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"bom_header",
     R"XML(<IPC-2581>
<Bom><BomHeader><StepRef/></BomHeader></Bom>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"bom_item",
     R"XML(<IPC-2581>
<Bom><BomItem><StepRef/></BomItem></Bom>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"bom_wrapper",
     R"XML(<IPC-2581>
<Bom><Wrapper><BomHeader><StepRef/></BomHeader></Wrapper></Bom>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"header_without_bom",
     R"XML(<IPC-2581>
<BomHeader><StepRef/></BomHeader>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"nested_step",
     R"XML(<IPC-2581>
<Bom><BomHeader><Wrapper><StepRef/></Wrapper></BomHeader></Bom>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"preserve_content",
     R"XML(<IPC-2581>
<Content><StepRef/></Content><Bom><StepRef/></Bom>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>

</Step></CadData></IPC-2581>
)XML",
     false},
    {"package_outline",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"side_placed",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"side_unplaced",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"side_mismatch",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="Q"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"side_no_location",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="P"/>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"mixed_allowed",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"multiple_side",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"wrong_side_ancestry",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><Wrapper><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Wrapper></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"invalid_area",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="0" y="0"/><PolyStepSegment x="0" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"unreferenced",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"features_only",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component><LayerFeature layerRef="Cu"><Set><Features><UserSpecial><LineDescRef id="line"/></UserSpecial></Features></Set></LayerFeature>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"mixed_invalid",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component><LayerFeature layerRef="Cu"><Set><Features><UserSpecial><LineDescRef id="line"/></UserSpecial></Features></Set></LayerFeature>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"mixed_dictionary",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc><EntryLineDesc id="unreferenced"><LineDesc/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"duplicate_entry",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc><EntryLineDesc id="line"><LineDesc/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"orphan_entry",
     R"XML(<IPC-2581>
<EntryLineDesc id="line"><LineDesc/></EntryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"empty_dictionary",
     R"XML(<IPC-2581>
<DictionaryLineDesc/>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"duplicate_package",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Package name="P"/><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"forward_side",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component><Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"plating_consumed",
     R"XML(<IPC-2581>

<CadHeader units="MILLIMETER"><Spec name="S"><General type="MATERIAL"><Property name="PLATING_THICKNESS" value="25" unit="MICRON"/></General></Spec></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/><Layer name="Drill" layerFunction="DRILL"><Span fromLayer="Cu" toLayer="Cu"/></Layer>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<LayerFeature layerRef="Drill"><Set><Hole name="V" diameter="1" x="2" y="2"><SpecRef id="S"/></Hole></Set></LayerFeature>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"duplicate_component",
     R"XML(<IPC-2581>
<DictionaryLineDesc><EntryLineDesc id="line"><LineDesc lineWidth="0"/></EntryLineDesc></DictionaryLineDesc>
<CadHeader units="MILLIMETER"></CadHeader>
<CadData>
<Layer name="Cu" layerFunction="CONDUCTOR"/>
<Stackup><StackupGroup><StackupLayer layerOrGroupRef="Cu" thickness="0.035" matDes="FR4"/></StackupGroup></Stackup>
<Step name="residual">
<Profile><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon></Profile>
<Package name="P"><OtherSideView><Outline><Polygon><PolyBegin x="0" y="0"/><PolyStepSegment x="4" y="0"/><PolyStepSegment x="4" y="3"/><PolyStepSegment x="0" y="3"/></Polygon><LineDescRef id="line"/></Outline></OtherSideView></Package><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component><Component refDes="J1" packageRef="P"><Location x="2" y="2"/></Component>
</Step></CadData></IPC-2581>
)XML",
     false},
    {"kicad4l", R"XML(tests/integration/ecad/data/odbpp/kicad4l.ipc2581.xml)XML", true},
    {"unsupported", R"XML(tests/integration/ecad/data/unsupported.xml)XML", true},
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
std::string report_bytes(
    const ecad::ImportReport& report, const Temporary& tmp, bool odb, bool accept, bool policy) {
    std::vector<chipcae::importer::ecad_defects::SkippedRow> skipped;
    auto defects = chipcae::importer::ecad_defects::classify_unsupported(report, skipped, odb);
    if (policy)
        defects = geometry::apply_repair_policy(std::move(defects), {}, accept).defects;
    std::string error;
    if (!geometry::write_import_report(tmp.path, {}, defects, &error))
        throw std::runtime_error(error);
    return bytes(tmp.path / "import_report.json");
}
struct Frozen {
    std::string input_sha, raw, classified, accepted, rejected, odb;
    std::map<std::string, std::string> promoted;
};
// Baseline capture: ea3da025c4deb11163a78f266887b6067527bcea, before implementation.
// All XML bytes, diagnostics, ordered IDs and complete report fields are frozen here.
const std::map<std::string, Frozen> snapshots =
    {
        {"color",
         {R"FROZEN(555e526ea6bcab0c04dcbaf28314307bade9ff97aafbc703450c6d69a41f82ee)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {{"board/Property/3", "spec-text"}}}},
        {"empty",
         {R"FROZEN(6911a4906108c1a5fbbc4c7b3f7ad1edc425dc1b542b34f1bb8b8f125a46ff5c)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {{"board/Property/3", "spec-text"}}}},
        {"empty_text",
         {R"FROZEN(19d9576a2956f58d85d53942de70c07e6a4c1bfc7c0d8dadd2dec22886a812ad)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {{"board/Property/3", "spec-text"}}}},
        {"whitespace",
         {R"FROZEN(20a1d6ec2f06958a8e8b88d30507ebb484edb63db1cf943f8879702b6e515a8c)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {{"board/Property/3", "spec-text"}}}},
        {"unicode",
         {R"FROZEN(2ea0141f04975abc04a48d204d8ec6b8ea71a44ff3823fdb1c7aebfaef7aec14)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {{"board/Property/3", "spec-text"}}}},
        {"same_line",
         {R"FROZEN(53658acd51e497386c83e21c1fe6ddae7ec3078aa8e739896e0b40e455fa4ef2)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
IMPORT_UNSUPPORTED_ELEMENT	Property	3
IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Property/3/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {{"board/Property/3", "spec-text"},
           {"board/Property/3/1", "spec-text"},
           {"board/Property/3/2", "spec-text"}}}},
        {"keyword_conduct",
         {R"FROZEN(4ff4a11404b57638f3455f931db5d60294ef81ad3ae52f11984cf1c43dc34fcb)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"keyword_thermal",
         {R"FROZEN(f9fb4a2db02d022f0913be13716e1c1b6ef0f744e777fbb14b0917b393bb5758)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"keyword_cte",
         {R"FROZEN(ce7956a6ab2f62a8837b43feb6514481a77bd4ad6b20c287363e6398548afee8)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"keyword_expansion",
         {R"FROZEN(402499ce915bd6edcce97cfcc1d95a9c09cab6dff55c128f62b18fdff908549d)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"keyword_modulus",
         {R"FROZEN(8f46b7c928128272dc570ba4724cdc6fe4c67a46e78997d04675c2c91e03bea9)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"keyword_young",
         {R"FROZEN(dde00c1fbf023668a9e21fd8d93c27fd9ba32086e0f1a17cef50bdbc2768b2d9)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"keyword_poisson",
         {R"FROZEN(92338e5f861c591451d8ea870d1a4c678a2187004c18d3001d17504ad7328637)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"keyword_heat",
         {R"FROZEN(f572888a701b6e101e5a48160a9e0693b2244689f2d72b51b321bea03826fd7c)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"keyword_density",
         {R"FROZEN(2b6643855ce26404cb1ed5221a2a1b44da378370d9ceda59555da45107a3ece8)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"keyword_thick",
         {R"FROZEN(3ad49dcea8333083de58e5fa65f3b1c381fb63dd5234981a4bb16263f409433a)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"digit_0",
         {R"FROZEN(5c8459237e7d2728e3c7763ecea08b18d6a9c05bfebaf1170d69662b167b2cc8)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"digit_1",
         {R"FROZEN(d290ecc22a66b76d6cfd23aa5fd42e9df5aea622c693c3bb7d0e52497db2c1af)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"digit_2",
         {R"FROZEN(223095f5d699d4dc3ea79fc64a7f56538081535fd8f634199f914d77a5a1bd55)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"digit_3",
         {R"FROZEN(73b8fe649bed2debf12c452f25980bd510939ea166cf49f73cc280cbd77540c4)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"digit_4",
         {R"FROZEN(2bec8031aa1f929742d49ad87a6910f0d9a314904485cfccfe9723d6541b83dd)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"digit_5",
         {R"FROZEN(9e40fafa039790bb813a0c21d9d77584d155f665558fb8943b59a687de4f5985)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"digit_6",
         {R"FROZEN(bb90b6b21a780fbca88c41f46a316967f9ede2cf0163548436e02b39ef00f919)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"digit_7",
         {R"FROZEN(de60d91f4cfc69972235129dd6928604eb2ebc7260344b50517cdcbec0da8d36)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"digit_8",
         {R"FROZEN(86bfcfe510689c2cf7b00b362787c58bb0bd9b9a9558f57a2136f11c64cdca59)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"digit_9",
         {R"FROZEN(5a50abf0a624bec35272f941907ab6db028b0fbf4f681c884ac4324ad2b4e2da)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"thickness",
         {R"FROZEN(a26efeb8233ec435310b1899e8019fc5fc3243640c7347598ffa864581b011b9)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"child",
         {R"FROZEN(dffdef0e2e11bbb4f929ea18e5f777c965509a5659444248024cdeb1218ec948)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
IMPORT_UNSUPPORTED_ELEMENT	Child	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":1},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"empty_child",
         {R"FROZEN(ee6494145151e2f96db34c43fd9179a4ae34acba9a3ccff1388db48494109feb)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
IMPORT_UNSUPPORTED_ELEMENT	Child	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":1},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"unit",
         {R"FROZEN(9df3f54ec97da9be880f46112030888ab1d6753b89bf3d4378163cc557a719f9)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"value",
         {R"FROZEN(80de513cf9b1e64dca31a64ec728d00b38e8c20ec1d1d9551d2a7340216da3cb)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"unknown_attribute",
         {R"FROZEN(e419d16f93014e0b598f31a2db441dc30f4298fd39059179b5eab041a4361367)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"other_general",
         {R"FROZEN(54cc415537966a2217f2ec8aaab9d791661d7be55d4098d71a1a00188d978b13)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"named_original",
         {R"FROZEN(f93eaddc6e3f990e55ec3b43d17148ca14db2ac593bd9dd5b4e9821adc93517a)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
IMPORT_UNSUPPORTED_ELEMENT	Child	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":1},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Child/3","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"named_value_unit",
         {R"FROZEN(a126560aa69d257fc8806098ff9ee7d7e9504287ccc501bbddfd4e25db64674f)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"wrong_property_ancestry",
         {R"FROZEN(315c07e4666b40dcf57e04c4ab6949720ec7bc84ab276c38744f3f8b4c9d9386)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Property	3
IMPORT_UNSUPPORTED_ELEMENT	Property	3
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Property/3","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/3/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"bom_header",
         {R"FROZEN(fe76d09854d8f9054988192ef5932ceede97e3cedfc7013375f7e476b63a7c8d)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	BomHeader	2
IMPORT_UNSUPPORTED_ELEMENT	StepRef	2
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":2},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {{"board/StepRef/2", "bom"}}}},
        {"bom_item",
         {R"FROZEN(65cf481af8c4ad79adb71b58e4f4c6ac7d3276e7fedb22481176e3a66a89e94e)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	BomItem	2
IMPORT_UNSUPPORTED_ELEMENT	StepRef	2
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomItem/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomItem/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomItem/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":2},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomItem/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"bom_wrapper",
         {R"FROZEN(6fd1f2d5afe4a9bad7d90538c58c46bd91230473d3965737c33b523ab04f652f)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	Wrapper	2
IMPORT_UNSUPPORTED_ELEMENT	BomHeader	2
IMPORT_UNSUPPORTED_ELEMENT	StepRef	2
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Wrapper/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/BomHeader/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Wrapper/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/BomHeader/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/Wrapper/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/BomHeader/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/Wrapper/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/BomHeader/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"header_without_bom",
         {R"FROZEN(09079644e5c86d9fd63ec084b31cba7c9d948a074d0bce373b8962be1ae37029)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	BomHeader	2
IMPORT_UNSUPPORTED_ELEMENT	StepRef	2
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":2},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"nested_step",
         {R"FROZEN(d10ec3415afd23f2a39d11305f8fa5e4e8857d1cef4ae251c8439606b0e6bb5e)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	BomHeader	2
IMPORT_UNSUPPORTED_ELEMENT	Wrapper	2
IMPORT_UNSUPPORTED_ELEMENT	StepRef	2
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Wrapper/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Wrapper/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Wrapper/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/BomHeader/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Wrapper/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StepRef/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"preserve_content",
         {R"FROZEN(47916ce4e8dc3ac26f80dad4377b155a14c9a91439c9479eb00f5da3c7a39cad)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	StepRef	2
IMPORT_UNSUPPORTED_ELEMENT	StepRef	2
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":2,"B":0,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/StepRef/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2/1","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":2,"B":0,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/StepRef/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2/1","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":2,"B":0,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/StepRef/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/2/1","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":2},"b_tier_accepted":null,"defects":[{"defect_id":"board/StepRef/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StepRef/2/1","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"package_outline",
         {R"FROZEN(88d45e9c549716496d36b3cc82fa585ef5d5dfa84e1c2ca9a6d0ca9fd470f281)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":2,"B":0,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":2,"B":0,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":2,"B":0,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":2},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"side_placed",
         {R"FROZEN(079e2a2fb0e4cba01830e00c3dcb9320e5973e588f16ead6678668cb18e0b203)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {{"board/DictionaryLineDesc/2", "def-instanced"},
           {"board/EntryLineDesc/2", "def-instanced"}}}},
        {"side_unplaced",
         {R"FROZEN(63dc3529444a9e5bb73c8234dce664d6af0849a87f82bc2595d4276295371eb6)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"side_mismatch",
         {R"FROZEN(5b7abdecb987925f44718a5174bd69b7c3f7a475bb74b10c688e5e03292fb6ab)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
IMPORT_UNSUPPORTED_ELEMENT	PackageRef:Q	0
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":4,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PackageRef%3AQ/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":4,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PackageRef%3AQ/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":4,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PackageRef%3AQ/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/PackageRef%3AQ/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"side_no_location",
         {R"FROZEN(f0bebc172f46dfeb75129fb76d18e107a7d6bf644fc84cf11c4f0d3b70217597)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"mixed_allowed",
         {R"FROZEN(92b80e41eef86675d69ab97f527b9d9539ccce6515e57a1a165ae898950a2a7f)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {{"board/DictionaryLineDesc/2", "def-instanced"},
           {"board/EntryLineDesc/2", "def-instanced"}}}},
        {"multiple_side",
         {R"FROZEN(99d1a099db51440860de3a8c6c50c3b7c428889c54305b9ead8490f6262f0088)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {{"board/DictionaryLineDesc/2", "def-instanced"},
           {"board/EntryLineDesc/2", "def-instanced"}}}},
        {"wrong_side_ancestry",
         {R"FROZEN(03904b4ff8719262d2ff9280344afbd7555b9ea39b767a7905844e00ff44a5b1)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	Wrapper	9
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":4,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Wrapper/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":4,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Wrapper/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":4,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Wrapper/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":4},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Wrapper/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"invalid_area",
         {R"FROZEN(fa4fd5eb12f2417643a13ff9e605e1127241ce44ce9a6695d9d3dd5c0efa2941)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"unreferenced",
         {R"FROZEN(3e4cdcfefc84f16afe4e32dbaf2f05686998a0af99183c2d847967f3e90e5175)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":2},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"features_only",
         {R"FROZEN(3c846978fb1397081bfed770ad923dc06c260f3d0c81572b35c87ede9addda66)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":2},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"mixed_invalid",
         {R"FROZEN(3938dc2dde4dd6970427a6b75a93721c1afa8b6843e7532015b7edef12454f2c)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"mixed_dictionary",
         {R"FROZEN(fa5640606a307ca1b5ce9a69ef402e65b044815e081c5eae2a38d5d5bf141745)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":4},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2/1","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {{"board/EntryLineDesc/2", "def-instanced"}}}},
        {"duplicate_entry",
         {R"FROZEN(00d98a5352ff76674f016140ed0a617cd6453bf5ba581b7d06e1cb415f39c090)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2/1","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":4},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2/1","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"orphan_entry",
         {R"FROZEN(f75c5b3d45b66825196e6ff3128dbdf12b4bce33517a80bd84457bc192e09d9f)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":2},"b_tier_accepted":null,"defects":[{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"empty_dictionary",
         {R"FROZEN(b87069df718854382d715937c39bbf66276672bc63ed5baf98d1881ce4902959)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":1,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":1},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"duplicate_package",
         {R"FROZEN(80a4c4046ceabb9485d87d8f9b11f82803b5b176e5b9e6c03296b986f4fbf75e)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"forward_side",
         {R"FROZEN(73cbb06553d0d9ac87b5ab92b9dacb5f34fb28915c294b05a79f11c8b68bc265)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":1,"B":2,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {{"board/DictionaryLineDesc/2", "def-instanced"},
           {"board/EntryLineDesc/2", "def-instanced"}}}},
        {"plating_consumed",
         {R"FROZEN(653c6b20e255fa143f9aaac1baebfcf4918c81a9881874422f9d3498c61c066d)FROZEN",
          R"FROZEN()FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":0},"b_tier_accepted":null,"defects":[]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":0},"b_tier_accepted":null,"defects":[]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":0},"b_tier_accepted":null,"defects":[]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":0},"b_tier_accepted":null,"defects":[]}
)FROZEN",
          {}}},
        {"duplicate_component",
         {R"FROZEN(e92fe93c18e159b59290e7ceecb58170d1f60f40cf692d89a28f22d98797a940)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	2
IMPORT_UNSUPPORTED_ELEMENT	OtherSideView	9
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/DictionaryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/OtherSideView/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
        {"kicad4l",
         {R"FROZEN(01a01e4509435d445452b68d929ab76ff4a0632c9d0a1725b80b1fc152d74016)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	SkippedLayer:F.Silkscreen:SILKSCREEN	0
IMPORT_UNSUPPORTED_ELEMENT	SkippedLayer:F.Paste:SOLDERPASTE	0
IMPORT_UNSUPPORTED_ELEMENT	SolderMask:F.Mask	0
IMPORT_UNSUPPORTED_ELEMENT	SolderMask:B.Mask	0
IMPORT_UNSUPPORTED_ELEMENT	SkippedLayer:B.Paste:SOLDERPASTE	0
IMPORT_UNSUPPORTED_ELEMENT	SkippedLayer:B.Silkscreen:SILKSCREEN	0
IMPORT_UNSUPPORTED_ELEMENT	FunctionMode	4
IMPORT_UNSUPPORTED_ELEMENT	StepRef	5
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	6
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	7
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	8
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	9
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	10
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	11
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	12
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	13
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	14
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	15
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	16
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	17
IMPORT_UNSUPPORTED_ELEMENT	LayerRef	18
IMPORT_UNSUPPORTED_ELEMENT	BomRef	19
IMPORT_UNSUPPORTED_ELEMENT	AvlRef	20
IMPORT_UNSUPPORTED_ELEMENT	DictionaryColor	21
IMPORT_UNSUPPORTED_ELEMENT	DictionaryLineDesc	22
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	23
IMPORT_UNSUPPORTED_ELEMENT	EntryLineDesc	26
IMPORT_UNSUPPORTED_ELEMENT	DictionaryFillDesc	30
IMPORT_UNSUPPORTED_ELEMENT	DictionaryStandard	31
IMPORT_UNSUPPORTED_ELEMENT	EntryStandard	32
IMPORT_UNSUPPORTED_ELEMENT	Oval	33
IMPORT_UNSUPPORTED_ELEMENT	EntryStandard	35
IMPORT_UNSUPPORTED_ELEMENT	Oval	36
IMPORT_UNSUPPORTED_ELEMENT	DictionaryUser	39
IMPORT_UNSUPPORTED_ELEMENT	EntryUser	40
IMPORT_UNSUPPORTED_ELEMENT	EntryUser	48
IMPORT_UNSUPPORTED_ELEMENT	Role	59
IMPORT_UNSUPPORTED_ELEMENT	Enterprise	60
IMPORT_UNSUPPORTED_ELEMENT	Person	61
IMPORT_UNSUPPORTED_ELEMENT	FileRevision	64
IMPORT_UNSUPPORTED_ELEMENT	SoftwarePackage	65
IMPORT_UNSUPPORTED_ELEMENT	Certification	66
IMPORT_UNSUPPORTED_ELEMENT	BomHeader	71
IMPORT_UNSUPPORTED_ELEMENT	StepRef	72
IMPORT_UNSUPPORTED_ELEMENT	BomItem	74
IMPORT_UNSUPPORTED_ELEMENT	RefDes	75
IMPORT_UNSUPPORTED_ELEMENT	Characteristics	76
IMPORT_UNSUPPORTED_ELEMENT	Textual	77
IMPORT_UNSUPPORTED_ELEMENT	Property	86
IMPORT_UNSUPPORTED_ELEMENT	Property	87
IMPORT_UNSUPPORTED_ELEMENT	Property	92
IMPORT_UNSUPPORTED_ELEMENT	Property	98
IMPORT_UNSUPPORTED_ELEMENT	Property	99
IMPORT_UNSUPPORTED_ELEMENT	Dielectric	101
IMPORT_UNSUPPORTED_ELEMENT	Property	102
IMPORT_UNSUPPORTED_ELEMENT	Conductor	109
IMPORT_UNSUPPORTED_ELEMENT	Property	110
IMPORT_UNSUPPORTED_ELEMENT	Property	116
IMPORT_UNSUPPORTED_ELEMENT	Dielectric	118
IMPORT_UNSUPPORTED_ELEMENT	Property	119
IMPORT_UNSUPPORTED_ELEMENT	Dielectric	121
IMPORT_UNSUPPORTED_ELEMENT	Property	122
IMPORT_UNSUPPORTED_ELEMENT	Conductor	129
IMPORT_UNSUPPORTED_ELEMENT	Property	130
IMPORT_UNSUPPORTED_ELEMENT	Property	136
IMPORT_UNSUPPORTED_ELEMENT	Dielectric	138
IMPORT_UNSUPPORTED_ELEMENT	Property	139
IMPORT_UNSUPPORTED_ELEMENT	Dielectric	141
IMPORT_UNSUPPORTED_ELEMENT	Property	142
IMPORT_UNSUPPORTED_ELEMENT	Conductor	149
IMPORT_UNSUPPORTED_ELEMENT	Property	150
IMPORT_UNSUPPORTED_ELEMENT	Property	156
IMPORT_UNSUPPORTED_ELEMENT	Dielectric	158
IMPORT_UNSUPPORTED_ELEMENT	Property	159
IMPORT_UNSUPPORTED_ELEMENT	Dielectric	161
IMPORT_UNSUPPORTED_ELEMENT	Property	162
IMPORT_UNSUPPORTED_ELEMENT	Conductor	169
IMPORT_UNSUPPORTED_ELEMENT	Property	170
IMPORT_UNSUPPORTED_ELEMENT	Property	176
IMPORT_UNSUPPORTED_ELEMENT	Property	177
IMPORT_UNSUPPORTED_ELEMENT	Dielectric	179
IMPORT_UNSUPPORTED_ELEMENT	Property	180
IMPORT_UNSUPPORTED_ELEMENT	Property	185
IMPORT_UNSUPPORTED_ELEMENT	Property	191
IMPORT_UNSUPPORTED_ELEMENT	Property	192
IMPORT_UNSUPPORTED_ELEMENT	NonstandardAttribute	276
IMPORT_UNSUPPORTED_ELEMENT	PadstackPadDef	278
IMPORT_UNSUPPORTED_ELEMENT	PadstackPadDef	282
IMPORT_UNSUPPORTED_ELEMENT	PadstackPadDef	286
IMPORT_UNSUPPORTED_ELEMENT	PadstackPadDef	290
IMPORT_UNSUPPORTED_ELEMENT	PadstackPadDef	297
IMPORT_UNSUPPORTED_ELEMENT	PadstackPadDef	301
IMPORT_UNSUPPORTED_ELEMENT	PadstackPadDef	305
IMPORT_UNSUPPORTED_ELEMENT	PadstackPadDef	309
IMPORT_UNSUPPORTED_ELEMENT	Datum	314
IMPORT_UNSUPPORTED_ELEMENT	PickupPoint	355
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	358
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	362
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	591
IMPORT_UNSUPPORTED_ELEMENT	PinRef	592
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	596
IMPORT_UNSUPPORTED_ELEMENT	PinRef	597
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	605
IMPORT_UNSUPPORTED_ELEMENT	PinRef	606
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	610
IMPORT_UNSUPPORTED_ELEMENT	PinRef	611
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	835
IMPORT_UNSUPPORTED_ELEMENT	PinRef	836
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	840
IMPORT_UNSUPPORTED_ELEMENT	PinRef	841
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	849
IMPORT_UNSUPPORTED_ELEMENT	PinRef	850
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	854
IMPORT_UNSUPPORTED_ELEMENT	PinRef	855
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	1079
IMPORT_UNSUPPORTED_ELEMENT	PinRef	1080
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	1084
IMPORT_UNSUPPORTED_ELEMENT	PinRef	1085
IMPORT_UNSUPPORTED_ELEMENT	NonstandardAttribute	1091
IMPORT_UNSUPPORTED_ELEMENT	Features	1092
IMPORT_UNSUPPORTED_ELEMENT	Polyline	1095
IMPORT_UNSUPPORTED_ELEMENT	Polyline	1107
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	1339
IMPORT_UNSUPPORTED_ELEMENT	PinRef	1340
IMPORT_UNSUPPORTED_ELEMENT	StandardPrimitiveRef	1344
IMPORT_UNSUPPORTED_ELEMENT	PinRef	1345
IMPORT_UNSUPPORTED_ELEMENT	Features	1351
IMPORT_UNSUPPORTED_ELEMENT	SlotCavity	1376
IMPORT_UNSUPPORTED_ELEMENT	Xform	1378
IMPORT_UNSUPPORTED_ELEMENT	Oval	1379
IMPORT_UNSUPPORTED_ELEMENT	Avl	1386
IMPORT_UNSUPPORTED_ELEMENT	AvlHeader	1387
IMPORT_UNSUPPORTED_ELEMENT	AvlItem	1388
IMPORT_UNSUPPORTED_ELEMENT	ViaPlatingMissing:F.Cu:B.Cu:1	0
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":74,"B":53,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/SolderMask%3AF.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/SolderMask%3AB.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/FunctionMode/4","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/5","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/6","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/7","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/8","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/10","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/11","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/12","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/13","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/14","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/15","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/16","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/17","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/18","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/BomRef/19","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/AvlRef/20","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryColor/21","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryLineDesc/22","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/23","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/26","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryFillDesc/30","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryStandard/31","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryStandard/32","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Oval/33","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryStandard/35","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Oval/36","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryUser/39","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryUser/40","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryUser/48","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Role/59","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Enterprise/60","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Person/61","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/FileRevision/64","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/SoftwarePackage/65","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Certification/66","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/BomHeader/71","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/72","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/BomItem/74","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/RefDes/75","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Characteristics/76","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Textual/77","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/86","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/87","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/92","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/98","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/99","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/101","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/102","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/109","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/110","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/116","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/118","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/119","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/121","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/122","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/129","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/130","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/136","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/138","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/139","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/141","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/142","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/149","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/150","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/156","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/158","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/159","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/161","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/162","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/169","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/170","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/176","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/177","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/179","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/180","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/185","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/191","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/192","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/NonstandardAttribute/276","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/278","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/282","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/286","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/290","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/297","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/301","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/305","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/309","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Datum/314","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PickupPoint/355","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/358","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/362","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/591","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/592","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/596","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/597","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/605","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/606","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/610","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/611","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/835","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/836","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/840","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/841","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/849","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/850","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/854","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/855","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1079","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1080","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1084","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1085","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/NonstandardAttribute/1091","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Features/1092","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Polyline/1095","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Polyline/1107","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1339","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1340","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1344","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1345","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Features/1351","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/SlotCavity/1376","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Xform/1378","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Oval/1379","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Avl/1386","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/AvlHeader/1387","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/AvlItem/1388","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/via_plating_missing/F.Cu-B.Cu","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm missing via plating","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":74,"B":53,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/SolderMask%3AF.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"solder mask not modeled","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/SolderMask%3AB.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"solder mask not modeled","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/FunctionMode/4","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/5","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/6","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/7","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/8","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/10","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/11","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/12","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/13","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/14","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/15","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/16","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/17","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/18","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/BomRef/19","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/AvlRef/20","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryColor/21","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryLineDesc/22","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/23","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/26","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryFillDesc/30","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryStandard/31","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryStandard/32","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Oval/33","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryStandard/35","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Oval/36","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/DictionaryUser/39","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryUser/40","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryUser/48","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Role/59","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Enterprise/60","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Person/61","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/FileRevision/64","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/SoftwarePackage/65","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Certification/66","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/BomHeader/71","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/72","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/BomItem/74","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/RefDes/75","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Characteristics/76","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Textual/77","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/86","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/87","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/92","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/98","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/99","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/101","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/102","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/109","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/110","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/116","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/118","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/119","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/121","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/122","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/129","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/130","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/136","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/138","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/139","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/141","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/142","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/149","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/150","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/156","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/158","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/159","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/161","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/162","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/169","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/170","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/176","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/177","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/179","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/180","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/185","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/191","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/192","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/NonstandardAttribute/276","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/278","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/282","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/286","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/290","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/297","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/301","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/305","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/309","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Datum/314","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PickupPoint/355","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/358","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/362","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/591","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/592","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/596","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/597","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/605","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/606","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/610","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/611","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/835","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/836","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/840","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/841","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/849","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/850","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/854","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/855","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1079","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1080","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1084","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1085","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/NonstandardAttribute/1091","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Features/1092","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Polyline/1095","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Polyline/1107","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1339","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1340","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1344","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1345","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Features/1351","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/SlotCavity/1376","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Xform/1378","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Oval/1379","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Avl/1386","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/AvlHeader/1387","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/AvlItem/1388","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/via_plating_missing/F.Cu-B.Cu","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm missing via plating","action":"confirm missing via plating","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":74,"B":53,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/SolderMask%3AF.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/SolderMask%3AB.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/FunctionMode/4","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/5","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/6","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/7","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/8","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/9","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/10","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/11","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/12","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/13","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/14","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/15","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/16","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/17","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/LayerRef/18","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/BomRef/19","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/AvlRef/20","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryColor/21","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryLineDesc/22","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/23","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/26","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/DictionaryFillDesc/30","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/DictionaryStandard/31","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryStandard/32","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Oval/33","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryStandard/35","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Oval/36","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/DictionaryUser/39","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryUser/40","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/EntryUser/48","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Role/59","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Enterprise/60","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Person/61","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/FileRevision/64","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/SoftwarePackage/65","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Certification/66","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/BomHeader/71","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StepRef/72","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/BomItem/74","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/RefDes/75","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Characteristics/76","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Textual/77","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/86","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Property/87","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Property/92","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Property/98","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Property/99","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Dielectric/101","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/102","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/109","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/110","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/116","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Dielectric/118","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/119","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/121","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/122","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/129","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/130","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/136","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Dielectric/138","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/139","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/141","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/142","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/149","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/150","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/156","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Dielectric/158","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/159","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/161","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/162","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/169","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/170","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/176","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Property/177","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Dielectric/179","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/180","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/185","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Property/191","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Property/192","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/NonstandardAttribute/276","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/278","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/282","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/286","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/290","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/297","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/301","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/305","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/309","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Datum/314","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PickupPoint/355","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"definition expressed by modeled instances","action":"definition expressed by modeled instances","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/358","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/362","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/591","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/592","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/596","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/597","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/605","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/606","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/610","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/611","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/835","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/836","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/840","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/841","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/849","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/850","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/854","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/855","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1079","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/1080","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1084","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/1085","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/NonstandardAttribute/1091","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Features/1092","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Polyline/1095","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Polyline/1107","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1339","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/1340","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1344","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/PinRef/1345","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Features/1351","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/SlotCavity/1376","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Xform/1378","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/Oval/1379","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Avl/1386","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/AvlHeader/1387","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/AvlItem/1388","tier":"A","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"non-physical metadata ignored","action":"non-physical metadata ignored","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/via_plating_missing/F.Cu-B.Cu","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm missing via plating","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":44,"C":86},"b_tier_accepted":null,"defects":[{"defect_id":"board/SolderMask%3AF.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"solder mask not modeled","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/SolderMask%3AB.Mask/0","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"solder mask not modeled","action":"solder mask not modeled","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/FunctionMode/4","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StepRef/5","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/6","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/7","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/8","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/9","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/10","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/11","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/12","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/13","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/14","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/15","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/16","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/17","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/LayerRef/18","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/BomRef/19","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/AvlRef/20","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/DictionaryColor/21","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/DictionaryLineDesc/22","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/23","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryLineDesc/26","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/DictionaryFillDesc/30","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/DictionaryStandard/31","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryStandard/32","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Oval/33","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryStandard/35","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Oval/36","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/DictionaryUser/39","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryUser/40","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/EntryUser/48","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Role/59","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Enterprise/60","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Person/61","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/FileRevision/64","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/SoftwarePackage/65","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Certification/66","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/BomHeader/71","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StepRef/72","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/BomItem/74","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/RefDes/75","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Characteristics/76","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Textual/77","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/86","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/87","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/92","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/98","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/99","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/101","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/102","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/109","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/110","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/116","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/118","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/119","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/121","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/122","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/129","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/130","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/136","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/138","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/139","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/141","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/142","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/149","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/150","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/156","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/158","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/159","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/161","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/162","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Conductor/169","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/170","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/176","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/177","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Dielectric/179","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Property/180","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/185","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/191","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/Property/192","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/NonstandardAttribute/276","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/278","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/282","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/286","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/290","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/297","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/301","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/305","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/PadstackPadDef/309","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Datum/314","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/PickupPoint/355","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/358","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/362","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/591","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/592","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/596","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/597","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/605","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/606","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/610","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/611","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/835","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/836","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/840","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/841","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/849","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/850","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/854","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/855","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1079","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1080","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1084","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1085","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/NonstandardAttribute/1091","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Features/1092","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Polyline/1095","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Polyline/1107","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1339","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1340","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/StandardPrimitiveRef/1344","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/PinRef/1345","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Features/1351","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/SlotCavity/1376","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Xform/1378","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Oval/1379","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/Avl/1386","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/AvlHeader/1387","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/AvlItem/1388","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/via_plating_missing/F.Cu-B.Cu","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm missing via plating","action":"confirm missing via plating","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          {{"board/StepRef/72", "bom"},
           {"board/Property/86", "spec-text"},
           {"board/Property/87", "spec-text"},
           {"board/Property/92", "spec-text"},
           {"board/Property/98", "spec-text"},
           {"board/Property/99", "spec-text"},
           {"board/Property/116", "spec-text"},
           {"board/Property/136", "spec-text"},
           {"board/Property/156", "spec-text"},
           {"board/Property/176", "spec-text"},
           {"board/Property/177", "spec-text"},
           {"board/Property/185", "spec-text"},
           {"board/Property/191", "spec-text"},
           {"board/Property/192", "spec-text"}}}},
        {"unsupported",
         {R"FROZEN(025c8a965028696c404e8fe4582bfd7329cc4015b72629da759818971d23c3c4)FROZEN",
          R"FROZEN(IMPORT_UNSUPPORTED_ELEMENT	MysteryA	2
IMPORT_UNSUPPORTED_ELEMENT	MysteryB	2
IMPORT_UNSUPPORTED_ELEMENT	MysteryC	2
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/MysteryA/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/MysteryB/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/MysteryC/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/MysteryA/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/MysteryB/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false},{"defect_id":"board/MysteryC/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"confirm omitted metadata, components or text","rejection_reason":"","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":3,"C":0},"b_tier_accepted":null,"defects":[{"defect_id":"board/MysteryA/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/MysteryB/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false},{"defect_id":"board/MysteryC/2","tier":"B","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"confirm omitted metadata, components or text","action":"","rejection_reason":"tier B defect requires explicit confirmation","material_ambiguous":false}]}
)FROZEN",
          R"FROZEN({"schema_version":"import-report/1.0","L":0,"t_min":0,"eps_weld":0,"tau_A":0,"tau_B":0,"delta_f":0,"tier_counts":{"Weld":0,"A":0,"B":0,"C":3},"b_tier_accepted":null,"defects":[{"defect_id":"board/MysteryA/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/MysteryB/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false},{"defect_id":"board/MysteryC/2","tier":"C","kind":"IMPORT_UNSUPPORTED_ELEMENT","reason":0,"dimension":"element","size":1,"size_unit":"1","suggestion":"unsupported geometry must be modeled before import","action":"","rejection_reason":"tier C defect is rejected","material_ambiguous":false}]}
)FROZEN",
          {}}},
};
std::string expected_report(std::string baseline, const Frozen& frozen, bool policy) {
    for (const auto& object : objects(baseline)) {
        const auto found = frozen.promoted.find(field(object, "defect_id"));
        if (found == frozen.promoted.end())
            continue;
        if (field(object, "tier") != "B")
            throw std::runtime_error("non-B baseline promotion");
        auto expected = object;
        const auto action = found->second == "def-instanced" ? definition_action : metadata_action;
        replace_field(expected, "tier", "A");
        replace_field(expected, "suggestion", action);
        if (policy) {
            replace_field(expected, "action", action);
            replace_field(expected, "rejection_reason", "");
        }
        baseline.replace(baseline.find(object), object.size(), expected);
    }
    // Tier counts are independently frozen in each report; only A/B may change.
    for (const auto& tier : {std::string("A"), std::string("B")}) {
        const auto begin = baseline.find("\"" + tier + "\":") + 4;
        const auto end = baseline.find_first_of(",}", begin);
        const auto old = std::stoi(baseline.substr(begin, end - begin));
        const auto delta = static_cast<int>(frozen.promoted.size()) * (tier == "A" ? 1 : -1);
        baseline.replace(begin, end - begin, std::to_string(old + delta));
    }
    return baseline;
}
const std::map<std::string, std::map<std::string, std::string>> geometry_hashes = {
    {"kicad4l",
     {{"stack.json", "a573c6f4f343a6ff73c2ced0fe46ea828430e8b60e99299a0ecafc909b959de8"},
      {"board.polyset", "d1e56d360fc1a4d2b7054bd1303fc2c8c71f2d8a58bb47001e2e6ed60e51c4b5"}}},
    {"side_placed",
     {{"stack.json", "864a2fb47aa6526925b36a330c6078451e7a57098dcb91c6bfd33bc4d7daa2e2"},
      {"board.polyset", "9c33ccd457542b80511340965443d6fc8539ec81f9724067deff1408d65f5463"}}},
};
const std::string mapping_sha = "ecd3aa2ec10f2162c30e198b5d30908104bba70f142c00580f43df61cb2dc2b2";
void verify(const Case& item) {
    const auto& frozen = snapshots.at(item.name);
    Temporary tmp;
    const auto input = item.fixture ? root / item.xml : tmp.write("board.xml", item.xml);
    check(chipcae::mesh::sha256(bytes(input)) == frozen.input_sha,
          item.name + ": frozen input SHA-256");
    const auto result = ecad::read_ipc2581(input);
    if (!result)
        throw std::runtime_error(item.name + ": " + result.error);
    if (item.name == "side_placed" || item.name == "mixed_allowed" ||
        item.name == "multiple_side") {
        check(result.model.components.size() == 1, item.name + ": one placed component");
        if (result.model.components.size() == 1) {
            const auto& points = result.model.components.front().outline.points;
            const std::vector<std::array<std::int64_t, 2>> polygon{
                {2000, 2000}, {6000, 2000}, {6000, 5000}, {2000, 5000}};
            auto expected_points = polygon;
            if (item.name != "side_placed")
                expected_points.insert(expected_points.end(), polygon.begin(), polygon.end());
            check(points == expected_points,
                  item.name + ": reference eligibility preserves parsed component geometry");
        }
    }
    std::string raw;
    for (const auto& row : result.model.report.unsupported)
        raw += row.code + "\t" + row.element + "\t" + std::to_string(row.line) + "\n";
    check(raw == frozen.raw, item.name + ": complete ordered raw diagnostics unchanged");
    const auto classified = report_bytes(result.model.report, tmp, false, true, false);
    const auto accepted = report_bytes(result.model.report, tmp, false, true, true);
    const auto rejected = report_bytes(result.model.report, tmp, false, false, true);
    const auto odb = report_bytes(result.model.report, tmp, true, true, true);
    const std::array<std::pair<std::string, std::string>, 3> reports{
        {{classified, expected_report(frozen.classified, frozen, false)},
         {accepted, expected_report(frozen.accepted, frozen, true)},
         {rejected, expected_report(frozen.rejected, frozen, true)}}};
    for (std::size_t mode = 0; mode < reports.size(); ++mode) {
        const auto actual = objects(reports[mode].first), expected = objects(reports[mode].second);
        check(actual.size() == expected.size(), item.name + ": defect count unchanged");
        for (std::size_t i = 0; i < std::min(actual.size(), expected.size()); ++i) {
            const auto id = field(expected[i], "defect_id");
            check(field(actual[i], "defect_id") == id, item.name + ": ordered ID " + id);
            if (frozen.promoted.contains(id))
                check(field(actual[i], "tier") == "A",
                      item.name + ": " + id + " expected B->A (" + frozen.promoted.at(id) +
                          "), got " + field(actual[i], "tier"));
            check(actual[i] == expected[i],
                  item.name + ": " + id + " complete defect fields, mode " + std::to_string(mode));
        }
        check(reports[mode].first == reports[mode].second,
              item.name + ": complete report, exact tier delta, Weld/C unchanged, mode " +
                  std::to_string(mode));
    }
    check(odb == frozen.odb, item.name + ": odb=true byte-identical baseline");
    if (geometry_hashes.contains(item.name)) {
        const auto mapping = root / "tests/integration/import_cli/material_map.json";
        check(chipcae::mesh::sha256(bytes(mapping)) == mapping_sha, "frozen material mapping");
        auto mapped = result.model;
        std::string error;
        if (!ecad::apply_material_map(mapped, mapping, &error) ||
            !ecad::to_stack(mapped, tmp.path / "stack.json", tmp.path / "board.polyset", &error))
            throw std::runtime_error(error);
        for (const auto& [file, digest] : geometry_hashes.at(item.name))
            check(chipcae::mesh::sha256(bytes(tmp.path / file)) == digest,
                  item.name + ": direct API geometry bytes unchanged: " + file);
    }
    if (item.name == "plating_consumed") {
        check(result.model.report.unsupported.empty(), "plating consumed without diagnostic");
        check(result.model.vias.size() == 1, "consumed plating still models the via");
        if (!result.model.vias.empty())
            check(std::abs(result.model.vias[0].inner_radius_dbu - 475) <= 1e-10,
                  "plating radius remains 475 dbu, absolute tolerance 1e-10");
    }
}
void verify_sibling_generals() {
    Temporary tmp;
    const auto input = tmp.write("sibling-generals.xml", R"XML(<IPC-2581>
<CadHeader units="MILLIMETER"><Spec name="S">
<General type="MATERIAL">
<Property text="FR4"/>
<Property text="green"/>
<Property name="COLOR" text="green"/>
</General>
<General type="OTHER">
<Property text="green"/>
<Property/>
<Property name="COLOR" text="green"/>
</General>
<General type="MATERIAL"><Property text="blue"/></General>
</Spec></CadHeader>
</IPC-2581>
)XML");
    const auto result = ecad::read_ipc2581(input);
    if (!result)
        throw std::runtime_error("sibling_generals: " + result.error);
    const std::array<std::tuple<std::string, std::string, std::string>, 6> expected{{
        {"board/Property/5", "A", "spec-text"},
        {"board/Property/6", "A", "spec-text"},
        {"board/Property/9", "B", ""},
        {"board/Property/10", "B", ""},
        {"board/Property/11", "B", ""},
        {"board/Property/13", "A", "spec-text"},
    }};
    const auto& unsupported = result.model.report.unsupported;
    check(unsupported.size() == expected.size(), "sibling_generals: material name stays consumed");
    for (const bool accept : {false, true}) {
        const auto defects = objects(report_bytes(result.model.report, tmp, false, accept, true));
        check(defects.size() == expected.size(), "sibling_generals: exact defect count");
        for (std::size_t i = 0; i < std::min(defects.size(), expected.size()); ++i) {
            const auto& [id, tier, context] = expected[i];
            const auto label = "sibling_generals: " + id;
            check(field(defects[i], "defect_id") == id, label + " ordered ID");
            check(field(defects[i], "tier") == tier, label + " tier");
            if (i < unsupported.size())
                check(unsupported[i].context == context, label + " General type context");
            const auto* suggestion = tier == "A" ? metadata_action : confirm;
            check(field(defects[i], "suggestion") == suggestion, label + " suggestion");
            check(field(defects[i], "action") == (tier == "A" || accept ? suggestion : ""),
                  label + " repair action");
            check(
                field(defects[i], "rejection_reason") ==
                    (tier == "B" && !accept ? "tier B defect requires explicit confirmation" : ""),
                label + " confirmation guard");
        }
    }
}
} // namespace
int main() {
    std::cout << "Frozen baseline: " << baseline_sha << '\n';
    for (const auto& input : inputs) {
        const auto before = failures;
        try {
            verify(input);
        } catch (const std::exception& error) {
            check(false, input.name + ": " + error.what());
        }
        std::cout << (before == failures ? "PASS " : "FAIL ") << input.name << '\n';
    }
    try {
        verify_sibling_generals();
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    std::cout << "Assertion failures: " << failures << '\n';
    return failures ? 1 : 0;
}
