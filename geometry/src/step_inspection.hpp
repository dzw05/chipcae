#pragma once

#include "chipcae/geometry/step_import.hpp"

#include <functional>

namespace chipcae::geometry::detail {
// Internal injection seam for checks that fail before inspection completes.
StepImportResult import_step_with_inspection(const std::filesystem::path&,
                                             const StepImportOptions&,
                                             const std::function<void()>& before_inspection);
} // namespace chipcae::geometry::detail

#ifdef CHIPCAE_ENABLE_OCCT

#include <ShapeBuild_ReShape.hxx>
#include <TDocStd_Document.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Shape.hxx>
#include <XSControl_WorkSession.hxx>
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
};
} // namespace chipcae::geometry::detail

#endif
