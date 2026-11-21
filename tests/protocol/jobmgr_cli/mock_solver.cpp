#include "chipcae/io/event_log.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    try {
        std::filesystem::path out;
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string_view(argv[i]) == "--out")
                out = argv[i + 1];
        if (out.empty())
            throw std::runtime_error("mock solver missing --out");
        // Deliberately invalid protobuf text: solver progress must not pollute CLI stdout.
        std::cout << "synthetic solver progress: 100%" << std::endl;
        chipcae::io::EventLogWriter writer(out / "events.bin");
        writer.append({1, 0, chipcae::io::KernelHello{}});
        writer.append({2, 0, chipcae::io::KernelExit{0, 2, "end_reached"}});
        writer.close();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mock solver: " << error.what() << '\n';
        return 96;
    }
}
