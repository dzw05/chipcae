#include "chipcae/io/checkpoint.hpp"

#include "chipcae/core/log.hpp"

#include <utility>

namespace chipcae::io {

Checkpoint::Checkpoint(std::filesystem::path directory) : directory_(std::move(directory)) {}

void Checkpoint::save(const std::string& tag) const {
    log(LogLevel::info, "Checkpoint::save is a placeholder: " + tag);
}

void Checkpoint::load(const std::string& tag) const {
    log(LogLevel::info, "Checkpoint::load is a placeholder: " + tag);
}

} // namespace chipcae::io
