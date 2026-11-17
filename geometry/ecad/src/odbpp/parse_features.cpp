#include "archive.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <sstream>

namespace chipcae::geometry::ecad::detail {
namespace {
template <typename T>
bool number_value(std::string_view text, T& value) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() &&
           std::isfinite(static_cast<double>(value));
}
bool exists(const ArchiveReader& reader, const std::string& path) {
    return std::any_of(reader.entries().begin(), reader.entries().end(), [&](const auto& e) {
        return e.path == path && !e.is_dir;
    });
}
bool finite(OdbPoint p) {
    return std::isfinite(p.x) && std::isfinite(p.y);
}
bool distinct(OdbPoint a, OdbPoint b) {
    return a.x != b.x || a.y != b.y;
}
bool arc_geometry(OdbPoint start, OdbPoint end, OdbPoint center) {
    if (!finite(start) || !finite(end) || !finite(center))
        return false;
    const double r = std::hypot(start.x - center.x, start.y - center.y);
    const double q = std::hypot(end.x - center.x, end.y - center.y);
    // Relative tolerance accommodates decimal roundoff without accepting a zero radius.
    return std::isfinite(r) && std::isfinite(q) && r > 0 && q > 0 &&
           std::abs(r - q) <= 1e-9 * std::max(r, q);
}
// Coordinates stay in job units in the frozen intermediate model.
bool records(const ArchiveReader& reader,
             const std::string& path,
             OdbJob& job,
             OdbFeatures* features,
             ImportResult& result) {
    OdbContour* contour = nullptr;
    OdbSurface* surface = nullptr;
    bool valid = true;
    std::uint64_t number = 0;
    std::map<int, std::size_t> symbol_ids;
    auto report = [&](const std::string& what) {
        result.model.report.unsupported.push_back(
            {"IMPORT_UNSUPPORTED_ELEMENT", path + ": " + what, number});
    };
    auto fail = [&](const std::string& why) {
        result.error = "IMPORT_REJECTED: " + why;
        valid = false;
        return false;
    };
    auto symbol = [&](int& index) {
        auto found = symbol_ids.find(index);
        if (found == symbol_ids.end()) {
            report("unknown symbol " + std::to_string(index));
            index = -1;
        } else
            index = static_cast<int>(found->second);
    };
    const bool ok = reader.for_each_line(
        path,
        [&](std::string_view raw) {
            ++number;
            // Most large files contain comments; avoid allocating or tokenizing them.
            auto first = raw.find_first_not_of(" \t\r");
            if (first == std::string_view::npos || raw[first] == '#')
                return true;
            auto line = trim(raw.substr(first));
            if (line.starts_with("UNITS=")) {
                auto units = lower(trim(std::string_view(line).substr(6)));
                if ((units != "mm" && units != "inch") ||
                    (units == "inch") != (job.units == OdbUnits::inch))
                    report("unit mismatch");
                return true;
            }
            std::istringstream in(line);
            std::string tag;
            in >> tag;
            if (tag == "F" || tag[0] == '@' || tag[0] == '&' || tag[0] == ';')
                return true;
            if (tag[0] == '$' && features) {
                int index = 0;
                if (!number_value(std::string_view(tag).substr(1), index))
                    return fail("malformed symbol index " + path);
                std::string name;
                in >> name;
                if (index < 0)
                    return fail("invalid symbol index " + path);
                if (symbol_ids.contains(index))
                    return fail("duplicate symbol index");
                symbol_ids.emplace(index, features->symbols.size());
                features->symbols.push_back({name});
                bool known = false;
                for (const auto* prefix : {"r", "oval", "rect", "s"})
                    if (name.starts_with(prefix) &&
                        name.size() > std::char_traits<char>::length(prefix) &&
                        std::isdigit(static_cast<unsigned char>(
                            name[std::char_traits<char>::length(prefix)])))
                        known = true;
                if (!known)
                    report("unsupported symbol " + name);
                return true;
            }
            if (contour && (tag == "S" || tag == "SE" || tag == "OB"))
                return fail("record while contour open " + path);
            if (tag == "S") {
                if (surface)
                    return fail("nested surface " + path);
                char polarity = 'P';
                in >> polarity;
                if (features) {
                    features->surfaces.push_back({polarity == 'P', {}});
                    surface = &features->surfaces.back();
                }
            } else if (tag == "OB") {
                OdbContour c;
                char kind = 'I';
                if (!(in >> c.start.x >> c.start.y >> kind))
                    return fail("malformed contour " + path);
                if (!finite(c.start))
                    return fail("non-finite contour " + path);
                c.is_hole = kind == 'H';
                auto& contours = features ? (surface ? surface->contours : job.profile.contours)
                                          : job.profile.contours;
                if (features && !surface)
                    return fail("contour outside surface " + path);
                contours.push_back(c);
                contour = &contours.back();
            } else if (tag == "OS" || tag == "OC") {
                if (!contour)
                    return fail("segment outside contour " + path);
                OdbSegment s;
                if (!(in >> s.end.x >> s.end.y))
                    return fail("malformed segment " + path);
                if (tag == "OC") {
                    char cw = 'N';
                    if (!(in >> s.center.x >> s.center.y >> cw))
                        return fail("malformed arc " + path);
                    s.is_arc = true;
                    s.cw = cw == 'Y';
                }
                const auto start =
                    contour->segs.empty() ? contour->start : contour->segs.back().end;
                if (!finite(s.end) || (s.is_arc && !finite(s.center)))
                    return fail("non-finite segment/arc " + path);
                if (!s.is_arc && !distinct(start, s.end))
                    return true;
                contour->segs.push_back(s);
            } else if (tag == "OE") {
                if (!contour)
                    return fail("contour end without start " + path);
                auto end = contour->segs.empty() ? contour->start : contour->segs.back().end;
                double scale = job.units == OdbUnits::mm ? 0.001 : 0.0254;
                if (!features && (std::abs(end.x - contour->start.x) * scale > 1e-9 ||
                                  std::abs(end.y - contour->start.y) * scale > 1e-9))
                    return fail("profile contour not closed");
                contour = nullptr;
            } else if (tag == "SE") {
                surface = nullptr;
            } else if (tag == "P" && features) {
                OdbPad p;
                char polarity = 'P';
                int dcode = 0, orientation = 0;
                if (!(in >> p.at.x >> p.at.y >> p.sym >> polarity >> dcode >> orientation))
                    return fail("malformed pad " + path);
                if (!finite(p.at) || !std::isfinite(p.rot_deg))
                    return fail("non-finite pad " + path);
                p.positive = polarity == 'P';
                if (orientation >= 0 && orientation <= 7) {
                    p.rot_deg = (orientation % 4) * 90;
                    p.mirror = orientation >= 4;
                } else if (orientation == 8 || orientation == 9) {
                    if (!(in >> p.rot_deg))
                        return fail("malformed pad angle " + path);
                    p.mirror = orientation == 9;
                } else
                    report("unknown pad orientation");
                if (!std::isfinite(p.rot_deg))
                    return fail("non-finite pad angle " + path);
                symbol(p.sym);
                features->pads.push_back(p);
            } else if (tag == "L" && features) {
                OdbLine l;
                char polarity = 'P';
                if (!(in >> l.s.x >> l.s.y >> l.e.x >> l.e.y >> l.sym >> polarity))
                    return fail("malformed line " + path);
                if (!finite(l.s) || !finite(l.e) || !distinct(l.s, l.e))
                    return fail("degenerate or non-finite line " + path);
                l.positive = polarity == 'P';
                symbol(l.sym);
                features->lines.push_back(l);
            } else if (tag == "A" && features) {
                OdbArc a;
                char polarity = 'P', cw = 'N';
                int dcode = 0;
                if (!(in >> a.s.x >> a.s.y >> a.e.x >> a.e.y >> a.c.x >> a.c.y >> a.sym >>
                      polarity >> dcode >> cw))
                    return fail("malformed arc " + path);
                if (!arc_geometry(a.s, a.e, a.c))
                    return fail("invalid arc geometry " + path);
                a.positive = polarity == 'P';
                a.cw = cw == 'Y';
                symbol(a.sym);
                features->arcs.push_back(a);
            } else
                report("unsupported record " + tag);
            return true;
        },
        result.error);
    if (!ok || !valid)
        return false;
    if (contour || surface) {
        result.error = "IMPORT_REJECTED: unterminated contour/surface " + path;
        return false;
    }
    if (!features && job.profile.contours.empty()) {
        result.error = "IMPORT_REJECTED: missing profile contours";
        return false;
    }
    return true;
}
} // namespace
bool parse_features(const ArchiveReader& reader, OdbJob& job, ImportResult& result) {
    if (!records(reader, "steps/" + job.step + "/profile", job, nullptr, result))
        return false;
    for (const auto& layer : job.layers) {
        auto base = layer_directory(reader, job, layer.name);
        if (reader.is_supported(base + "features") && exists(reader, base + "features") &&
            !records(reader, base + "features", job, &job.features[layer.name], result))
            return false;
        if (!reader.is_supported(base + "tools") || !exists(reader, base + "tools"))
            continue;
        auto& tools = job.tools[layer.name];
        OdbTool tool;
        bool active = false;
        bool valid = true;
        std::uint64_t number = 0;
        if (!reader.for_each_line(
                base + "tools",
                [&](std::string_view raw) {
                    ++number;
                    auto line = trim(raw);
                    if (line.starts_with("TOOLS") && line.find('{') != std::string::npos) {
                        active = true;
                        tool = {};
                        return true;
                    }
                    if (line == "}" && active) {
                        tools.push_back(tool);
                        active = false;
                        return true;
                    }
                    auto eq = line.find('=');
                    if (eq == std::string::npos)
                        return true;
                    auto key = trim(std::string_view(line).substr(0, eq)),
                         value = trim(std::string_view(line).substr(eq + 1));
                    if (key == "UNITS" &&
                        ((lower(value) == "inch") != (job.units == OdbUnits::inch)))
                        result.model.report.unsupported.push_back(
                            {"IMPORT_UNSUPPORTED_ELEMENT", base + "tools: unit mismatch", number});
                    if (active) {
                        if ((key == "NUM" && !number_value(value, tool.num)) ||
                            (key == "FINISH_SIZE" && !number_value(value, tool.finish_size))) {
                            result.error =
                                "IMPORT_REJECTED: malformed tool " + key + " " + base + "tools";
                            valid = false;
                            return false;
                        }
                        if (key == "TYPE")
                            tool.type = value;
                    }
                    return true;
                },
                result.error))
            return false;
        if (!valid)
            return false;
        if (active) {
            result.error = "IMPORT_REJECTED: unterminated tools " + base + "tools";
            return false;
        }
    }
    return true;
}
} // namespace chipcae::geometry::ecad::detail
