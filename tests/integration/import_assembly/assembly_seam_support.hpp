#pragma once
#include "chipcae/geometry/diagnostics.hpp"
#include "chipcae/geometry/import_report.hpp"
#include "chipcae/schema/stack.hpp"

#include <string>
#include <vector>

// Private orchestration seams for deterministic synthetic diagnostics. Including
// the TU avoids exporting these helpers as installed geometry APIs.
#include "../../../kernel/src/import_assembly.cpp"
#include "../../../kernel/src/import_common.cpp"
