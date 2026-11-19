#include "assembly_seam_support.hpp"

int main() {
    chipcae::geometry::StepImportResult result;
    result.body_names = {"ordinary-body"};
    if (chipcae::importer::assembly_display_only(result))
        return 1;
    // The imported product hierarchy carries the root name even when the
    // ordinary filename and selected body do not contain the marker.
    result.body_names.push_back("NOT_CAE_GEOMETRY/ordinary-body");
    if (!chipcae::importer::assembly_display_only(result))
        return 1;
    result.body_names.clear();
    return chipcae::importer::assembly_display_only(result) ? 1 : 0;
}
