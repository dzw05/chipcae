#pragma once

#ifdef CHIPCAE_ENABLE_ECAD
#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "chipcae/geometry/import_report.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace chipcae::importer::ecad_defects {
namespace ecad = chipcae::geometry::ecad;
using chipcae::geometry::Defect;
using chipcae::geometry::Tier;
// Percent encoding is injective, stable across source relocation and independent
// of standard-library hashes. Line numbers distinguish repeated source records.
inline std::string id_component(const std::string& value) {
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
struct SkippedRow {
    std::string name, function;
};
inline std::vector<Defect>
classify_unsupported(const ecad::ImportReport& report, std::vector<SkippedRow>& skipped, bool odb) {
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
        const auto colon = text.find(':');
        const auto tag = text.substr(0, colon);
        // Unknown ODB diagnostics stay C. Only IPC can supply bare XML
        // extension names without geometry context.
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
                                                    "StandardPrimitiveRef",
                                                    "selected step"};
        static const std::set<std::string> geometry{"Arc",
                                                    "Circle",
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
        // Match complete ODB paths and records, not substrings that can also
        // occur in unknown geometry diagnostics (for example record TT).
        auto path = tag;
        if (path.ends_with(".Z") || path.ends_with(".z"))
            path.resize(path.size() - 2);
        std::vector<std::string> parts;
        for (const auto& part : std::filesystem::path(path))
            parts.push_back(part.string());
        const bool layer_file = parts.size() == 5 && parts[0] == "steps" && !parts[1].empty() &&
                                parts[2] == "layers" && !parts[3].empty();
        const bool text_record = odb && colon != std::string::npos && layer_file &&
                                 parts[4] == "features" &&
                                 text.substr(colon) == ": unsupported record T";
        const bool metadata_path =
            odb && ((layer_file && parts[4] == "components") || path == "misc/info" ||
                    path == "misc/attrlist" || text.starts_with("selected step "));
        const bool ipc_extension = !odb && colon == std::string::npos;
        if (!geometry.contains(tag) &&
            (metadata.contains(tag) || ipc_extension || text_record || metadata_path)) {
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
                    defect.defect_id = "board/via_plating_missing/" +
                                       text.substr(start, end - start) + "-" +
                                       text.substr(end + 1, number - end - 1);
                }
            }
            // ODB's shorter diagnostic identifies the drill layer, with no
            // span/count available; keep its stable board ID and size=1.
        }
        const auto count = occurrences[defect.defect_id]++;
        if (count)
            defect.defect_id += "/" + std::to_string(count);
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

inline std::vector<geometry::Defect> board_defects(const geometry::ecad::ImportReport& report,
                                                   bool odb) {
    std::vector<SkippedRow> skipped;
    return classify_unsupported(report, skipped, odb);
}
} // namespace chipcae::importer::ecad_defects
#endif
