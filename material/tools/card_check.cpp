#include "chipcae/material/schema.hpp"

#include <cstdio>
#include <string_view>
int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: material_card_check card.json [--delivery]\n");
        return 2;
    }
    auto result = chipcae::material::validate_material_file(
        argv[1], argc > 2 && std::string_view(argv[2]) == "--delivery");
    for (auto& issue : result.issues)
        std::fprintf(stderr, "%s: %s\n", issue.field_path.c_str(), issue.message.c_str());
    return result.ok ? 0 : 1;
}
