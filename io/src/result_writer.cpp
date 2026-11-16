#include "chipcae/io/result_writer.hpp"

#include "chipcae/core/log.hpp"

#include <utility>

namespace chipcae::io {

ResultWriter::ResultWriter(std::filesystem::path directory) : directory_(std::move(directory)) {}

void ResultWriter::write_field(const Field& field) const {
    log(LogLevel::info, "ResultWriter::write_field is a placeholder: " + field.name());
}

} // namespace chipcae::io
