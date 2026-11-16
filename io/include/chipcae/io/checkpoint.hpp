#pragma once

#include <filesystem>
#include <string>

namespace chipcae::io {

class Checkpoint {
public:
    explicit Checkpoint(std::filesystem::path directory);

    const std::filesystem::path& directory() const noexcept { return directory_; }
    void save(const std::string& tag) const;
    void load(const std::string& tag) const;

private:
    std::filesystem::path directory_;
};

} // namespace chipcae::io
