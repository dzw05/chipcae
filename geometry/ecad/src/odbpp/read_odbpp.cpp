#include "archive.hpp"

#include <algorithm>
namespace chipcae::geometry::ecad::detail {
ImportResult read_odbpp_job(const std::filesystem::path& input, OdbJob& job) {
    ImportResult result;
    result.model.id = "odbpp";
    job = {};
    try {
        ArchiveReader reader(input);
        result.model.report.unsupported = reader.unsupported();
        result.error = reader.open_error();
        if (!result.error.empty())
            return result;
        if (reader.entries().empty() && !reader.unsupported().empty())
            return result;
        if (!parse_matrix(reader, job, result.error))
            return result;
        std::vector<std::string> steps;
        for (const auto& entry : reader.entries()) {
            if (!entry.path.starts_with("steps/"))
                continue;
            auto tail = entry.path.substr(6);
            auto slash = tail.find('/');
            if (slash == std::string::npos && !entry.is_dir)
                continue;
            auto name = tail.substr(0, slash);
            if (!name.empty() && std::find(steps.begin(), steps.end(), name) == steps.end())
                steps.push_back(name);
        }
        if (steps.empty()) {
            result.error = "IMPORT_REJECTED: no step";
            return result;
        }
        auto pcb = std::find(steps.begin(), steps.end(), "pcb");
        job.step = pcb == steps.end() ? steps.front() : *pcb;
        if (pcb == steps.end() || steps.size() > 1)
            result.model.report.unsupported.push_back(
                {"IMPORT_UNSUPPORTED_ELEMENT", "selected step " + job.step, 0});
        if (!parse_misc(reader, job, result.error))
            return result;
        parse_features(reader, job, result);
    } catch (const std::exception& e) {
        result.error = "IMPORT_REJECTED: " + std::string(e.what());
    }
    return result;
}
} // namespace chipcae::geometry::ecad::detail
namespace chipcae::geometry::ecad {
ImportResult read_odbpp(const std::filesystem::path& input) {
    detail::OdbJob job;
    return detail::read_odbpp_job(input, job);
}
} // namespace chipcae::geometry::ecad
