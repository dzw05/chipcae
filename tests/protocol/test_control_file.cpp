#include "chipcae/io/control_file.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main() {
    const auto path = std::filesystem::temp_directory_path() / "chipcae-control-file-test.jsonl";
    std::filesystem::remove(path);
    chipcae::io::ControlFileReader reader(path);
    auto append = [&](const std::string& s) { std::ofstream(path, std::ios::app) << s; };
    append("{\"cmd_seq\":\"1\",\"action\":\"PAUSE\"}\n{\"cmd_seq\":\"2\",\"action\":\"RESUME\"}\n");
    auto first = reader.read_available();
    if (first.size() != 2 || first[0].action != chipcae::io::ControlAction::pause ||
        first[1].cmd_seq != 2)
        throw std::runtime_error("control append/read failed");
    append("{\"cmd_seq\":\"2\",\"action\":\"STOP\"}\n{\"cmd_seq\":\"3\",\"action\":\"STOP\"");
    auto second = reader.read_available();
    if (!second.empty())
        throw std::runtime_error("duplicate or partial command was consumed");
    append("}\n");
    auto third = reader.read_available();
    if (third.size() != 1 || third.front().cmd_seq != 3 ||
        third.front().action != chipcae::io::ControlAction::stop)
        throw std::runtime_error("completed partial command was not consumed");
    std::filesystem::remove(path);
    std::cout << "control file OK\n";
}
