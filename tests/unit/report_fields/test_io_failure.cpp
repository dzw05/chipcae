#include "chipcae/geometry/import_report.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace chipcae::geometry;

namespace {
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() /
                             ("chipcae-reportfields-" + std::to_string(random()) + "-" +
                              std::to_string(random()));
            if (std::filesystem::create_directory(candidate)) {
                path = candidate;
                return;
            }
        }
        throw std::runtime_error("cannot create exclusive temporary directory");
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
} // namespace

int main() {
    try {
        for (bool nested : {false, true}) {
            TemporaryDirectory temporary;
            const auto blocker = temporary.path / "ordinary-file";
            {
                std::ofstream file(blocker);
                file << "blocked";
                if (!file)
                    throw std::runtime_error("cannot create blocker");
            }
            std::string error;
            const auto directory = nested ? blocker / "reports" : blocker;
            if (write_import_report(directory, {}, {}, &error))
                throw std::runtime_error("blocked report directory unexpectedly succeeded");
            if (error.empty())
                throw std::runtime_error("blocked report directory returned an empty error");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
