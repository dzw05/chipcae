#pragma once

#ifdef CHIPCAE_ENABLE_ECAD
#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "chipcae/geometry/import_report.hpp"

#include <string>
#include <vector>

namespace chipcae::importer::ecad_defects {
struct SkippedRow {
    std::string name, function;
};

// TEST-FIRST compilation stubs. No importer includes this header yet.
inline std::vector<geometry::Defect>
classify_unsupported(const geometry::ecad::ImportReport&, std::vector<SkippedRow>&, bool) {
    return {};
}

inline std::vector<geometry::Defect> board_defects(const geometry::ecad::ImportReport&, bool) {
    return {};
}
} // namespace chipcae::importer::ecad_defects
#endif
