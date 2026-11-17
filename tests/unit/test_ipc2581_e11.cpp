#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace ecad = chipcae::geometry::ecad;
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
ecad::EcadModel parse(const std::string& xml) {
    const auto file =
        std::filesystem::temp_directory_path() /
        ("ipc-e11-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".xml");
    {
        std::ofstream out(file);
        out << xml;
    }
    auto result = ecad::read_ipc2581(file);
    std::filesystem::remove(file);
    check(bool(result), result.error.c_str());
    return std::move(result.model);
}
void vias() {
    for (int mode = 0; mode < 3; ++mode) {
        std::ostringstream xml;
        xml << "<IPC-2581><CadHeader units='MICRON'/><CadData><Step name='vias'>";
        // Uses precede both layer declarations and padstack definitions.
        for (int kind = 0; kind < 2; ++kind) {
            if (mode != 2 && mode != kind)
                continue;
            xml << "<LayerFeature layerRef='drill" << kind << "'><Set>";
            for (int i = 0; i < 100; ++i)
                xml << "<Pad padstackDefRef='via'><Location x='" << (i % 10) * 1000 + kind * 12000
                    << "' y='" << (i / 10) * 1000 << "'/></Pad>";
            xml << "</Set></LayerFeature>";
        }
        xml << "<PadStackDef name='via'><PadstackHoleDef name='hole' diameter='200' "
               "platingStatus='VIA'/></PadStackDef>"
               "</Step><Stackup><StackupGroup>";
        for (auto name : {"outside", "top", "d1", "middle", "d2", "bottom", "outside2"})
            xml << "<StackupLayer layerOrGroupRef='" << name << "' thickness='100'/>";
        xml << "</StackupGroup></Stackup>";
        // SIGNAL declarations deliberately follow the stackup.
        for (auto name : {"top", "middle", "bottom"})
            xml << "<Layer name='" << name << "' layerFunction='SIGNAL'/>";
        xml << "<Layer name='drill0' layerFunction='DRILL'><SpecRef id='solid'/><Span "
               "fromLayer='top' toLayer='bottom'/></Layer>"
               "<Layer name='drill1' layerFunction='DRILL'><SpecRef id='hollow'/><Span "
               "fromLayer='bottom' toLayer='middle'/></Layer>"
               "<Spec name='solid'><General type='MATERIAL'><Property name='PLATING_THICKNESS' "
               "value='100' unit='MICRON'/></General></Spec>"
               "<Spec name='hollow'><General type='MATERIAL'><Property name='PLATING_THICKNESS' "
               "value='20' unit='MICRON'/></General></Spec></CadData></IPC-2581>";
        auto model = parse(xml.str());
        check(model.vias.size() == (mode == 2 ? 200u : 100u), "via count");
        check(model.report.unsupported.empty(), "supported via subset reported unsupported");
        check(model.via_fractions.size() == (mode == 1 ? 1u : 2u),
              "unexpected dielectric contribution");
        for (const auto& v : model.vias) {
            check(v.start_layer == (v.position[0] < 12000 ? "top" : "bottom"), "start ref");
            check(v.dielectric_layers == (v.position[0] < 12000 ? std::vector<std::size_t>{2, 4}
                                                                : std::vector<std::size_t>{4}),
                  "span");
        }
        std::vector<ecad::ViaFraction> cells;
        for (auto layer : {0u, 2u, 4u, 6u})
            for (int x = 0; x < 24; ++x)
                cells.push_back({layer, {-500.0 + x * 1000, -500}, {500.0 + x * 1000, 9500}, 0, 0});
        check(ecad::rasterize_vias(model, cells), "rasterize");
        for (auto layer : {0u, 2u, 4u, 6u}) {
            double actual = 0;
            std::size_t count = 0;
            for (const auto& c : cells)
                if (c.layer == layer) {
                    actual += c.fraction * 1e7;
                    count += c.via_count;
                }
            const bool solid = (mode == 0 || mode == 2) && (layer == 2 || layer == 4);
            const bool hollow = (mode == 1 || mode == 2) && layer == 4;
            const double expected =
                100 * std::acos(-1.0) * ((solid ? 10000 : 0) + (hollow ? 3600 : 0));
            check(count == (solid ? 100u : 0u) + (hollow ? 100u : 0u), "contribution count");
            const double error = expected ? std::abs(actual - expected) / expected : actual;
            std::cout << "mode=" << mode << " layer=" << layer << " area=" << actual
                      << " relative_error=" << error << '\n';
            check(error <= 1e-4, "integrated area");
            for (const auto& c : model.via_fractions)
                if (c.layer == layer) {
                    const double imported_area =
                        c.fraction * (c.upper[0] - c.lower[0]) * (c.upper[1] - c.lower[1]);
                    check(expected > 0 && std::abs(imported_area - expected) / expected <= 1e-4,
                          "imported fraction area");
                    check(c.via_count == count, "imported contribution count");
                }
        }
        auto single = model;
        single.vias.resize(1);
        const auto& v = single.vias.front();
        const double x = v.position[0], y = v.position[1];
        std::vector<ecad::ViaFraction> halves{
            {v.dielectric_layers.front(), {x - 100, y - 100}, {x, y + 100}, 0, 0},
            {v.dielectric_layers.front(), {x, y - 100}, {x + 100, y + 100}, 0, 0},
            {v.dielectric_layers.front(), {x - 10, y - 10}, {x + 10, y + 10}, 0, 0}};
        check(ecad::rasterize_vias(single, halves), "partial cells");
        const double half_area =
            std::acos(-1.0) * (10000 - v.inner_radius_dbu * v.inner_radius_dbu) / 2;
        for (std::size_t i = 0; i < 2; ++i)
            check(std::abs(halves[i].fraction * 20000 - half_area) / half_area <= 1e-4,
                  "clipped half area");
        check(std::abs(halves[2].fraction - (mode == 1 ? 0.0 : 1.0)) < 1e-12,
              "hole or fully filled cell");
        // Duplicating a conductor must preserve union area, even for annuli.
        auto duplicate = model;
        duplicate.vias.insert(duplicate.vias.end(), model.vias.begin(), model.vias.end());
        auto repeated = cells;
        check(ecad::rasterize_vias(duplicate, repeated), "duplicate raster");
        for (std::size_t i = 0; i < cells.size(); ++i)
            check(std::abs(cells[i].fraction - repeated[i].fraction) < 1e-12, "overlap union");
        std::vector<ecad::ViaFraction> invalid{{1, {0, 0}, {1, 1}, 0, 0}};
        check(!ecad::rasterize_vias(model, invalid), "copper cells accepted");
    }
}
void drill_holes() {
    auto model =
        parse("<IPC-2581><CadHeader units='INCH'/><LayerFeature layerRef='drill'><Set>"
              "<Hole name='H1' diameter='0.02' platingStatus='PLATED' x='1' y='2'><SpecRef "
              "id='wall'/></Hole>"
              "<Hole name='NP' diameter='0.02' platingStatus='NONPLATED' x='0' y='0'/>"
              "<Pad padstackDefRef='missing'><Location x='0' y='0'/></Pad></Set></LayerFeature>"
              "<StackupLayer layerOrGroupRef='a'/><StackupLayer layerOrGroupRef='d'/><StackupLayer "
              "layerOrGroupRef='b'/>"
              "<Layer name='a' layerFunction='SIGNAL'/><Layer name='b' layerFunction='PLANE'/>"
              "<Layer name='drill' layerFunction='DRILL'><Span fromLayer='a' toLayer='b'/></Layer>"
              "<Spec name='wall'><General type='MATERIAL'><Property name='PLATING_THICKNESS' "
              "value='25.4' unit='MICRON'/></General></Spec></IPC-2581>");
    check(model.vias.size() == 1 && model.vias[0].id == "H1", "Hole/nonplated import");
    check(model.vias[0].position == std::array<std::int64_t, 2>{25400, 50800}, "inch location");
    check(std::abs(model.vias[0].outer_radius_dbu - 254) < 1e-9 &&
              std::abs(model.vias[0].inner_radius_dbu - 228.6) < 1e-9,
          "inch radii");
    check(model.report.unsupported.size() == 1 &&
              model.report.unsupported[0].element == "PadstackRef:missing",
          "missing reference report");
}
void components() {
    auto model = parse(
        "<IPC-2581><CadHeader units='MILLIMETER'/><Step>"
        "<Component refDes='U1' packageRef='pkg'><Xform rotation='90'/><Location x='10' "
        "y='20'/></Component>"
        "<Component refDes='U2' packageRef='pkg'><Xform rotation='30'/><Location x='-1.99951' "
        "y='3.00049'/></Component>"
        "<Package name='pkg'><Outline><Polygon><PolyBegin x='0' y='0'/><PolyStepSegment "
        "x='2' y='0'/>"
        "<PolyStepSegment x='2' y='1'/><PolyStepSegment x='0' y='1'/></Polygon></Outline>"
        "<Pin number='1'><Location x='0.50049' y='0.25049'/></Pin><Pin number='2'><Location "
        "x='1.5' "
        "y='0.75'/></Pin>"
        "</Package></Step></IPC-2581>");
    check(model.components.size() == 2, "component count");
    const std::array<std::int64_t, 2> origins[] = {{{10000, 20000}}, {{-2000, 3000}}};
    const std::array<double, 2> source_origins[] = {{{10000, 20000}}, {{-1999.51, 3000.49}}};
    const double angles[] = {90, 30};
    double max_error = 0;
    const std::array<double, 2> local[] = {
        {{0, 0}}, {{2000, 0}}, {{2000, 1000}}, {{0, 1000}}, {{500.49, 250.49}}, {{1500, 750}}};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto& c = model.components[i];
        check(c.id == "U" + std::to_string(i + 1) && c.package == "pkg", "component identifiers");
        check(c.position == origins[i] && c.rotation_degrees == angles[i], "placement");
        check(c.outline.points.size() == 4 && c.pads.size() == 2, "outline/pad count");
        for (std::size_t j = 0; j < 6; ++j) {
            auto actual = j < 4 ? c.outline.points[j] : c.pads[j - 4].position;
            const double t = angles[i] * std::acos(-1.0) / 180;
            const double expected[] = {
                source_origins[i][0] + local[j][0] * std::cos(t) - local[j][1] * std::sin(t),
                source_origins[i][1] + local[j][0] * std::sin(t) + local[j][1] * std::cos(t)};
            for (int axis = 0; axis < 2; ++axis) {
                const double error = std::abs(actual[axis] - expected[axis]);
                max_error = std::max(max_error, error);
                check(error <= 1, "coordinate exceeds one dbu");
            }
        }
        check(c.pads[0].id == "1" && c.pads[1].id == "2", "pin identifiers");
    }
    std::cout << "2 components, 4 pads, 12 transformed points; max coordinate error=" << max_error
              << " dbu\n";
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "mode required");
        if (std::string(argv[1]) == "vias") {
            vias();
            drill_holes();
        } else
            components();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
