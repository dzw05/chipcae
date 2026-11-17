#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace chipcae::geometry::ecad::detail {
enum class OdbUnits { mm, inch };
struct OdbLayer {
    std::string name, context, type, polarity;
    int row = 0;
    std::string start_name, end_name;
}; // matrix/matrix 的 LAYER 块；start/end 只对钻孔层有值
struct OdbAttrs {
    std::map<std::string, std::string> job;
    std::map<std::string, std::map<std::string, std::string>> layer;
}; // misc/attrlist 与 layers/<层>/attrlist，键值原样
struct OdbPoint {
    double x = 0, y = 0;
};
struct OdbSegment {
    OdbPoint end;
    bool is_arc = false;
    OdbPoint center;
    bool cw = false;
}; // OS / OC 记录
struct OdbContour {
    bool is_hole = false;
    OdbPoint start;
    std::vector<OdbSegment> segs;
}; // OB…OE；弧不离散
struct OdbProfile {
    std::vector<OdbContour> contours;
};
struct OdbSymbol {
    std::string name;
}; // $n 符号表，名字原样，由 E-ODB-2 解释
struct OdbPad {
    OdbPoint at;
    int sym = -1;
    bool positive = true;
    double rot_deg = 0;
    bool mirror = false;
};
struct OdbLine {
    OdbPoint s, e;
    int sym = -1;
    bool positive = true;
};
struct OdbArc {
    OdbPoint s, e, c;
    int sym = -1;
    bool positive = true;
    bool cw = false;
};
struct OdbSurface {
    bool positive = true;
    std::vector<OdbContour> contours;
};
struct OdbFeatures {
    std::vector<OdbSymbol> symbols;
    std::vector<OdbPad> pads;
    std::vector<OdbLine> lines;
    std::vector<OdbArc> arcs;
    std::vector<OdbSurface> surfaces;
};
struct OdbTool {
    int num = 0;
    std::string type;
    double finish_size = 0;
}; // layers/<钻孔层>/tools
struct OdbJob {
    OdbUnits units = OdbUnits::mm;
    std::string step;
    std::vector<OdbLayer> layers;
    OdbAttrs attrs;
    OdbProfile profile;
    std::map<std::string, OdbFeatures> features;
    std::map<std::string, std::vector<OdbTool>> tools;
}; // features/tools 的键是层名

} // namespace chipcae::geometry::ecad::detail
