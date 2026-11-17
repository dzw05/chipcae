#pragma once

// Private process exit contract shared by the entrypoint and format runners.
namespace chipcae::importer {
struct CliExit {
    static constexpr int success = 0;
    static constexpr int usage = 2;
    static constexpr int import_rejected = 7;
    static constexpr int material_mapping_missing = 9;
};

} // namespace chipcae::importer
