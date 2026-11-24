#include "chipcae/io/event_log.hpp"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

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
        if (const auto* value = std::getenv("CHIPCAE_CLI_MOCK_SLEEP_MS")) {
            const std::string_view text(value);
            std::chrono::milliseconds::rep delay = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), delay);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || delay < 0)
                throw std::runtime_error("invalid mock sleep milliseconds");
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
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
