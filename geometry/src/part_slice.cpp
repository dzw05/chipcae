#include "chipcae/geometry/part_slice.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

#ifdef CHIPCAE_ENABLE_OCCT
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <GProp_GProps.hxx>
#include <ShapeBuild_ReShape.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <XSControl_WorkSession.hxx>
#include <gp_Pln.hxx>

namespace chipcae::geometry::detail {
// The importer owns this private representation. Keep this definition identical
// to step_import.cpp until the internal representation can move to a header.
struct StepShape {
    TopoDS_Shape shape;
    TopTools_IndexedMapOfShape faces;
    Handle(ShapeBuild_ReShape) history = new ShapeBuild_ReShape;
    Handle(TDocStd_Document) document;
    Handle(XSControl_WorkSession) session;
    std::vector<std::string> entities;
    std::vector<std::string> names;
};
} // namespace chipcae::geometry::detail
#endif

namespace chipcae::geometry {
namespace {
SliceResult reject(DefectReason reason, const char* details) {
    Defect d{};
    d.tier = Tier::C;
    d.reason = reason;
    d.details = details;
    return {{}, {d}};
}
#ifdef CHIPCAE_ENABLE_OCCT
using Point = std::array<Real, 2>;
Real distance(Point a, Point b) {
    return std::hypot(a[0] - b[0], a[1] - b[1]);
}
Real area(const Ring& r) {
    Real a = 0;
    for (std::size_t i = 0; i < r.pts.size(); ++i) {
        auto p = r.pts[i], q = r.pts[(i + 1) % r.pts.size()];
        a += p[0] * q[1] - q[0] * p[1];
    }
    return a / 2;
}
void canonical(Ring& r, bool positive) {
    if ((area(r) > 0) != positive)
        std::reverse(r.pts.begin(), r.pts.end());
    std::rotate(r.pts.begin(), std::min_element(r.pts.begin(), r.pts.end()), r.pts.end());
}
bool inside(Point p, const Ring& r) {
    bool yes = false;
    for (std::size_t i = 0, j = r.pts.size() - 1; i < r.pts.size(); j = i++) {
        auto a = r.pts[i], b = r.pts[j];
        if ((a[1] > p[1]) != (b[1] > p[1]) &&
            p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0])
            yes = !yes;
    }
    return yes;
}
using Polylines = std::vector<std::vector<Point>>;
Polylines section_edges(const TopoDS_Shape& shape, Real z, Real chord) {
    // OCCT's intersection tolerance can otherwise include a nearby horizontal
    // cap or a wall ending at a neighbouring slab boundary.
    BRep_Builder builder;
    TopoDS_Compound crossing;
    builder.MakeCompound(crossing);
    for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
        auto face = TopoDS::Face(it.Current());
        BRepAdaptor_Surface surface(face);
        if (surface.GetType() == GeomAbs_Plane &&
            std::abs(surface.Plane().Axis().Direction().Z()) >= 1 - 1e-9 &&
            surface.Plane().Location().Z() != z)
            continue;
        Bnd_Box bounds;
        BRepBndLib::AddOptimal(face, bounds, false, false);
        Real x0, y0, z0, x1, y1, z1;
        bounds.Get(x0, y0, z0, x1, y1, z1);
        if (z >= z0 && z <= z1)
            builder.Add(crossing, face);
    }
    BRepAlgoAPI_Section section(crossing, gp_Pln(gp_Pnt(0, 0, z), gp_Dir(0, 0, 1)), false);
    section.Build();
    if (!section.IsDone())
        throw std::runtime_error("section failed");
    std::vector<std::vector<Point>> edges;
    for (TopExp_Explorer it(section.Shape(), TopAbs_EDGE); it.More(); it.Next()) {
        BRepAdaptor_Curve curve(TopoDS::Edge(it.Current()));
        std::vector<Point> points;
        if (curve.GetType() == GeomAbs_Line) {
            for (Real t : {curve.FirstParameter(), curve.LastParameter()}) {
                auto p = curve.Value(t);
                points.push_back({p.X(), p.Y()});
            }
        } else {
            GCPnts_QuasiUniformDeflection sample(curve, chord * 0.1);
            if (!sample.IsDone())
                throw std::runtime_error("curve sampling failed");
            for (int i = 1; i <= sample.NbPoints(); ++i) {
                auto p = sample.Value(i);
                points.push_back({p.X(), p.Y()});
            }
        }
        edges.push_back(std::move(points));
    }
    return edges;
}
std::vector<Ring> contours(const TopoDS_Shape& shape, Real z, Real chord, Real eps) {
    auto edges = section_edges(shape, z, chord);
    std::vector<Ring> rings;
    while (!edges.empty()) {
        Ring r{std::move(edges.back())};
        edges.pop_back();
        while (distance(r.pts.front(), r.pts.back()) > eps) {
            bool found = false;
            for (auto it = edges.begin(); it != edges.end(); ++it) {
                if (distance(r.pts.back(), it->back()) <= eps)
                    std::reverse(it->begin(), it->end());
                if (distance(r.pts.back(), it->front()) <= eps) {
                    r.pts.insert(r.pts.end(), it->begin() + 1, it->end());
                    edges.erase(it);
                    found = true;
                    break;
                }
            }
            if (!found)
                throw std::runtime_error("open section contour");
        }
        r.pts.pop_back();
        if (r.pts.size() >= 3) {
            canonical(r, true);
            rings.push_back(std::move(r));
        }
    }
    std::sort(rings.begin(), rings.end(), [](const Ring& a, const Ring& b) {
        return std::abs(area(a)) > std::abs(area(b));
    });
    return rings;
}
std::vector<SlabSection> sections(std::vector<Ring> rings) {
    std::vector<SlabSection> out;
    for (std::size_t i = 0; i < rings.size(); ++i) {
        int depth = 0;
        for (std::size_t j = 0; j < i; ++j)
            if (inside(rings[i].pts.front(), rings[j]))
                ++depth;
        if (depth % 2 == 0)
            out.push_back({rings[i], {}});
        else {
            auto hole = rings[i];
            canonical(hole, false);
            for (auto it = out.rbegin(); it != out.rend(); ++it)
                if (inside(hole.pts.front(), it->outer)) {
                    it->holes.push_back(std::move(hole));
                    break;
                }
        }
    }
    const auto less = [](const Ring& a, const Ring& b) { return a.pts < b.pts; };
    for (auto& s : out)
        std::sort(s.holes.begin(), s.holes.end(), less);
    std::sort(out.begin(), out.end(), [&](const auto& a, const auto& b) {
        return less(a.outer, b.outer);
    });
    return out;
}
Real point_segment(Point p, Point a, Point b) {
    Real dx = b[0] - a[0], dy = b[1] - a[1], den = dx * dx + dy * dy;
    Real t =
        den ? std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / den, Real(0), Real(1)) : 0;
    return distance(p, {a[0] + t * dx, a[1] + t * dy});
}
// Directed distance from sampled contour vertices to the other contour's
// segments. Reverse the comparison for symmetric Hausdorff distance.
Real deviation(const Polylines& a, const Polylines& b) {
    Real maximum = 0;
    for (const auto& r : a)
        for (auto p : r) {
            Real minimum = std::numeric_limits<Real>::infinity();
            for (const auto& line : b)
                for (std::size_t i = 1; i < line.size(); ++i)
                    minimum = std::min(minimum, point_segment(p, line[i - 1], line[i]));
            maximum = std::max(maximum, minimum);
        }
    return maximum;
}
bool equal(const std::vector<SlabSection>& a, const std::vector<SlabSection>& b) {
    auto ring = [](const Ring& x, const Ring& y) {
        if (x.pts.size() != y.pts.size())
            return false;
        for (std::size_t i = 0; i < x.pts.size(); ++i)
            if (std::abs(x.pts[i][0] - y.pts[i][0]) > 1e-12 ||
                std::abs(x.pts[i][1] - y.pts[i][1]) > 1e-12)
                return false;
        return true;
    };
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!ring(a[i].outer, b[i].outer) || a[i].holes.size() != b[i].holes.size())
            return false;
        for (std::size_t j = 0; j < a[i].holes.size(); ++j)
            if (!ring(a[i].holes[j], b[i].holes[j]))
                return false;
    }
    return true;
}
#endif
} // namespace
SliceResult
slice_prismatic(const StepImportResult& input, std::string_view body, const SliceOptions& options) {
#ifndef CHIPCAE_ENABLE_OCCT
    (void)input;
    (void)body;
    (void)options;
    return reject(DefectReason::OCCT_UNAVAILABLE, "");
#else
    if (body.empty() ||
        std::find(input.body_names.begin(), input.body_names.end(), body) == input.body_names.end())
        return reject(DefectReason::PART_NOT_PRISMATIC, "body not found");
    if (!input.source)
        return reject(DefectReason::PART_NOT_PRISMATIC, "empty shape");
    try {
        BRep_Builder builder;
        TopoDS_Compound shape;
        builder.MakeCompound(shape);
        int count = 0;
        for (int i = 1; i <= input.source->faces.Extent(); ++i)
            if (input.source->names.at(i - 1) == body) {
                builder.Add(shape, input.source->faces(i));
                ++count;
            }
        if (!count)
            return reject(DefectReason::PART_NOT_PRISMATIC, "empty shape");
        if (!std::isfinite(options.chord_tol) || options.chord_tol <= 0 ||
            !std::isfinite(options.tau_a) || !std::isfinite(options.tau_b) || options.tau_a < 0 ||
            options.tau_b < options.tau_a)
            return reject(DefectReason::PART_NOT_PRISMATIC, "invalid slice options");
        Bnd_Box box;
        BRepBndLib::AddOptimal(shape, box, false, false);
        Real x0, y0, z0, x1, y1, z1;
        box.Get(x0, y0, z0, x1, y1, z1);
        Real height = z1 - z0, L = std::hypot(x1 - x0, y1 - y0, height);
        if (height <= 0)
            return reject(DefectReason::PART_NOT_PRISMATIC, "empty shape");
        auto tol = compute_tolerances({L, std::min({x1 - x0, y1 - y0, height}), L, 0}, {});
        if (!tol)
            return reject(DefectReason::PART_NOT_PRISMATIC, "degenerate bounds");
        Real eps = tol.value().eps_weld;
        std::vector<Real> planes{z0, z1};
        bool horizontal = false;
        Real largest_area = 0, largest_cosine = 0;
        for (TopExp_Explorer it(shape, TopAbs_FACE); it.More(); it.Next()) {
            auto face = TopoDS::Face(it.Current());
            BRepAdaptor_Surface s(face);
            if (s.GetType() != GeomAbs_Plane)
                continue;
            Real cosine = std::abs(s.Plane().Axis().Direction().Z());
            GProp_GProps properties;
            BRepGProp::SurfaceProperties(face, properties);
            if (properties.Mass() > largest_area) {
                largest_area = properties.Mass();
                largest_cosine = cosine;
            }
            if (cosine >= 1 - 1e-9) {
                horizontal = true;
                planes.push_back(s.Plane().Location().Z());
            }
        }
        if (!horizontal && largest_area > 0 &&
            std::acos(std::clamp(largest_cosine, Real(0), Real(1))) >
                std::atan(options.tau_a / height))
            return reject(DefectReason::PART_TILTED, "largest planar face normal tilted from z");
        std::sort(planes.begin(), planes.end());
        planes.erase(
            std::unique(planes.begin(), planes.end(), [&](Real a, Real b) { return b - a <= eps; }),
            planes.end());
        SliceResult result;
        Real delta = 0;
        Real volume_error = 0;
        for (std::size_t i = 1; i < planes.size(); ++i) {
            Real low = planes[i - 1], high = planes[i], mid = (low + high) / 2;
            auto center = contours(shape, mid, options.chord_tol, eps);
            if (center.empty())
                continue;
            Real slab_delta = 0;
            auto reference = section_edges(shape, mid, options.chord_tol);
            for (Real z : {low + options.tau_a, high - options.tau_a}) {
                if (high - low < 3 * options.tau_a)
                    break;
                auto sample = section_edges(shape, z, options.chord_tol);
                if (sample.empty())
                    return reject(DefectReason::PART_NOT_PRISMATIC, "empty sampled section");
                slab_delta = std::max(
                    {slab_delta, deviation(sample, reference), deviation(reference, sample)});
            }
            delta = std::max(delta, slab_delta);
            Real perimeter = 0;
            for (const auto& ring : center)
                for (std::size_t j = 0; j < ring.pts.size(); ++j)
                    perimeter += distance(ring.pts[j], ring.pts[(j + 1) % ring.pts.size()]);
            // First-order swept boundary estimate, reported for B confirmation.
            volume_error += perimeter * slab_delta * (high - low);
            auto cross = sections(std::move(center));
            if (!result.slabs.empty() && result.slabs.back().z1 == low &&
                equal(result.slabs.back().sections, cross))
                result.slabs.back().z1 = high;
            else
                result.slabs.push_back({low, high, std::move(cross)});
        }
        if (result.slabs.empty())
            return reject(DefectReason::PART_NOT_PRISMATIC, "empty section");
        if (delta > options.tau_a) {
            Defect d{};
            d.tier = delta <= options.tau_b ? Tier::B : Tier::C;
            d.reason = DefectReason::PART_NOT_PRISMATIC;
            d.dimension = "vertical deviation";
            d.size = delta;
            std::ostringstream report;
            report.precision(17);
            report << "maximum section Hausdorff distance; estimated volume error (m^3): "
                   << volume_error;
            d.details = report.str();
            result.defects.push_back(d);
        }
        return result;
    } catch (const Standard_Failure&) {
        return reject(DefectReason::PART_NOT_PRISMATIC, "section geometry failed");
    } catch (const std::exception&) {
        return reject(DefectReason::PART_NOT_PRISMATIC, "section contour failed");
    }
#endif
}
} // namespace chipcae::geometry
