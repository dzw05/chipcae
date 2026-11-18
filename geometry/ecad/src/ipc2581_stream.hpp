#pragma once

#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <memory>
#include <string_view>

namespace chipcae::geometry::ecad::detail {
class Ipc2581StreamParser {
public:
    Ipc2581StreamParser();
    ~Ipc2581StreamParser();
    bool feed(std::string_view chunk);
    ImportResult finish();

private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace chipcae::geometry::ecad::detail
