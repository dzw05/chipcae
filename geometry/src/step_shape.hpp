#pragma once

#include <ShapeBuild_ReShape.hxx>
#include <TDocStd_Document.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Shape.hxx>
#include <XSControl_WorkSession.hxx>
#include <map>
#include <string>
#include <vector>

namespace chipcae::geometry::detail {
struct StepShape {
    TopoDS_Shape shape;
    TopTools_IndexedMapOfShape faces;
    Handle(ShapeBuild_ReShape) history = new ShapeBuild_ReShape;
    Handle(TDocStd_Document) document;
    Handle(XSControl_WorkSession) session;
    std::vector<std::string> entities;
    std::vector<std::string> names;
    std::map<std::string, TopoDS_Shape, std::less<>> bodies;
};
} // namespace chipcae::geometry::detail
