// Collected by chipcae_auto_tests: proves test_*.cpp discovery and the source-dir define.
#include <cstdio>
#include <string>

int main() {
    const std::string src = CHIPCAE_SOURCE_DIR;
    if (src.empty()) {
        std::fprintf(stderr, "CHIPCAE_SOURCE_DIR is empty\n");
        return 1;
    }
    std::printf("autoglob cpp ok: %s\n", src.c_str());
    return 0;
}
