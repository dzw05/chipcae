#include "chipcae/geometry/ecad/odbpp.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() /
                     ("chipcae-odbpp-" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        if (!fs::create_directory(dir)) {
            std::cerr << "Could not create temporary empty directory\n";
            return 1;
        }
        const auto result = chipcae::geometry::ecad::read_odbpp(dir);
        fs::remove_all(dir);
        if (result || !result.error.starts_with("IMPORT_REJECTED: not implemented")) {
            std::cerr
                << "Expected failed ImportResult with IMPORT_REJECTED: not implemented; got ok="
                << static_cast<bool>(result) << " error='" << result.error << "'\n";
            return 1;
        }
    } catch (const std::exception& e) {
        fs::remove_all(dir);
        std::cerr << "read_odbpp empty-directory check threw: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
