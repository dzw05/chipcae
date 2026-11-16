#include "chipcae/core/version.hpp"
#include "chipcae/jobmgr/job_manager.hpp"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--version" || arg == "-v") {
            std::cout << "chipcae-jobmgr " << chipcae::version_string() << '\n';
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "usage: chipcae-jobmgr [--version]\n";
            return 0;
        }
    }

    chipcae::jobmgr::JobManager manager;
    (void)manager;
    std::cout << "chipcae-jobmgr skeleton is idle\n";
    return 0;
}
