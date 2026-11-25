#include "chipcae/io/control_file.hpp"
#include "chipcae/io/event_log.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <jsoncons/json.hpp>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>

using namespace jobmgr_test;
using Json = jsoncons::json;

int main(int argc, char** argv) {
    try {
        std::vector<std::string> args(argv, argv + argc);
        const auto option = [&](std::string_view name) {
            const auto found = std::find(args.begin(), args.end(), name);
            require(found != args.end() && std::next(found) != args.end(), "missing mock argument");
            return *std::next(found);
        };
        const auto profile = Json::parse(read(option("--profile")));
        const fs::path out = option("--out");
        const auto mode = profile.get_value_or<std::string>("x-fixture", "success");
        if (mode == "sleep" || mode == "ignore-term")
            require(std::signal(SIGTERM, mode == "ignore-term" ? SIG_IGN : SIG_DFL) != SIG_ERR,
                    "install SIGTERM handler");
        Json capture(jsoncons::json_object_arg);
        capture["argv"] = args;
        capture["pid"] = static_cast<long>(getpid());
        capture["cwd"] = fs::current_path().string();
        capture["omp"] = std::getenv("OMP_NUM_THREADS") ? std::getenv("OMP_NUM_THREADS") : "";
        capture["sentinel"] =
            std::getenv("CHIPCAE_JOBMGR_SENTINEL") ? std::getenv("CHIPCAE_JOBMGR_SENTINEL") : "";
        write(out / "mock-argv.tmp", capture.to_string());
        fs::rename(out / "mock-argv.tmp", out / "mock-argv.json");
        if (mode == "sleep" || mode == "ignore-term") {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            return 0;
        }
        if (mode == "crash") {
            const rlimit limit{0, 0};
            require(setrlimit(RLIMIT_CORE, &limit) == 0, "disable core dumps");
            std::raise(SIGSEGV);
            return 98;
        }
        const auto file = out / profile.get_value_or<std::string>("x-events-file", "events.bin");
        if (mode == "control") {
            const auto timeout =
                std::chrono::milliseconds(profile.get_value_or<int>("x-control-timeout-ms", 5000));
            chipcae::io::EventLogWriter writer(file);
            std::uint64_t sequence = 0;
            writer.append({++sequence, 0, chipcae::io::KernelHello{}});
            chipcae::io::ControlFileReader reader(out / "control.jsonl");
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline) {
                for (const auto& command : reader.read_available()) {
                    const bool stop = command.action == chipcae::io::ControlAction::stop;
                    const bool checkpoint =
                        command.action == chipcae::io::ControlAction::checkpoint_and_stop;
                    writer.append({++sequence,
                                   0,
                                   chipcae::io::ControlAck{
                                       command.cmd_seq,
                                       command.client_request_id,
                                       stop || checkpoint ? 1u : 3u,
                                       0,
                                       stop || checkpoint ? "mock STOP" : "mock supports STOP only",
                                       0}});
                    if (stop || checkpoint) {
                        writer.append({++sequence,
                                       0,
                                       chipcae::io::KernelExit{
                                           0, 0, stop ? "user_stop" : "checkpoint_and_stop"}});
                        writer.close();
                        return 0;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            writer.append({++sequence, 0, chipcae::io::KernelExit{0, 0, "end_reached"}});
            writer.close();
            return 0;
        }
        if (mode != "missing") {
            {
                chipcae::io::EventLogWriter writer(file);
                writer.append({1, 0, chipcae::io::KernelHello{}});
                if (mode != "no-exit")
                    writer.append({2,
                                   0,
                                   chipcae::io::KernelExit{mode == "failed-exit" ? 7 : 0,
                                                           2,
                                                           profile.get_value_or<std::string>(
                                                               "x-reason", "end_reached")}});
            }
            if (mode == "torn" || mode == "invalid") {
                auto bytes = read(file);
                bytes += mode == "torn" ? "CAE" : bytes;
                if (mode == "invalid")
                    bytes.back() ^= 1;
                write(file, bytes);
            }
        }
        return mode == "abnormal" ? 9 : 0;
    } catch (const std::exception& e) {
        std::cerr << "mock solver: " << e.what() << '\n';
        return 96;
    }
}
