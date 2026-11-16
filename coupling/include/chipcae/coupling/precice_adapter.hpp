#pragma once

#include <string>
#include <string_view>

namespace chipcae::coupling {

/// Adapter that will expose chipcae fields to preCICE. Not wired yet.
class PreciceAdapter {
public:
    explicit PreciceAdapter(std::string participant_name);

    const std::string& participant_name() const noexcept { return participant_name_; }
    bool is_connected() const noexcept { return false; }
    void initialize();
    void finalize();

private:
    std::string participant_name_;
};

} // namespace chipcae::coupling
