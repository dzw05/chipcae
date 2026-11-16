#pragma once

#include "chipcae/core/field.hpp"

#include <filesystem>
#include <string>

namespace chipcae::io {

class ResultWriter {
public:
    explicit ResultWriter(std::filesystem::path directory);

    const std::filesystem::path& directory() const noexcept { return directory_; }
    void write_field(const Field& field) const;

private:
    std::filesystem::path directory_;
};

} // namespace chipcae::io
