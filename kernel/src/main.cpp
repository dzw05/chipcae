#include "chipcae/core/log.hpp"
#include "chipcae/core/version.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--version" || arg == "-v") {
            std::cout << "chipcae-kernel " << chipcae::version_string() << '\n';
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "usage: chipcae-kernel [--version]\n";
            return 0;
        }
        chipcae::log(chipcae::LogLevel::error, "unknown argument");
        return 2;
    }

    chipcae::log(chipcae::LogLevel::info, "chipcae-kernel skeleton is ready; no run was requested");
    return 0;
}
