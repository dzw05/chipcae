#include "chipcae/core/version.hpp"

#include <charconv>
#include <chrono>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

#if CHIPCAE_JOBMGR_LOCAL
#include "chipcae/jobmgr/local_job_manager.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <google/protobuf/text_format.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#endif

namespace {
constexpr std::string_view usage =
    "usage:\n"
    "  chipcae-jobmgr --help\n"
    "  chipcae-jobmgr -h\n"
    "  chipcae-jobmgr --version\n"
    "  chipcae-jobmgr -v\n"
    "  chipcae-jobmgr --root <dir> submit --case <case.json> --profile <profile.json> "
    "--request-id <uuid> [--solver <path>] [--mpi <path>] [--wait [--timeout-ms <n>]]\n"
    "  chipcae-jobmgr --root <dir> list\n"
    "  chipcae-jobmgr --root <dir> get --run-id <uuid>\n"
    "  chipcae-jobmgr --root <dir> wait --run-id <uuid> [--timeout-ms <n>]\n"
    "  chipcae-jobmgr --root <dir> cancel --run-id <uuid> [--grace-ms <n>]\n"
    "  chipcae-jobmgr --root <dir> poll\n";

class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Arguments {
    std::string_view root;
    std::string_view command;
    std::map<std::string_view, std::string_view> options;
    std::chrono::milliseconds timeout{0};
    std::chrono::milliseconds grace{0};
};

bool allowed(std::string_view command, std::string_view option) {
    if (command == "submit")
        return option == "--case" || option == "--profile" || option == "--request-id" ||
               option == "--solver" || option == "--mpi" || option == "--wait" ||
               option == "--timeout-ms";
    if (command == "get")
        return option == "--run-id";
    if (command == "wait")
        return option == "--run-id" || option == "--timeout-ms";
    if (command == "cancel")
        return option == "--run-id" || option == "--grace-ms";
    return false;
}

std::chrono::milliseconds milliseconds(std::string_view value) {
    std::chrono::milliseconds::rep count = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw UsageError("Invalid milliseconds: " + std::string(value));
    return std::chrono::milliseconds(count);
}

Arguments parse(int argc, char** argv) {
    if (argc < 4 || std::string_view(argv[1]) != "--root")
        throw UsageError("Expected --root <dir> and a command");
    Arguments result;
    result.root = argv[2];
    if (result.root.empty() || result.root.starts_with("--"))
        throw UsageError("Missing value for --root");
    result.command = argv[3];
    const auto command = result.command;
    if (command != "submit" && command != "list" && command != "get" && command != "wait" &&
        command != "cancel" && command != "poll")
        throw UsageError("Unknown command: " + std::string(command));
    for (int i = 4; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (!allowed(command, option))
            throw UsageError("Unexpected argument: " + std::string(option));
        if (result.options.contains(option))
            throw UsageError("Duplicate option: " + std::string(option));
        std::string_view value;
        if (option != "--wait") {
            if (i + 1 >= argc)
                throw UsageError("Missing value for " + std::string(option));
            value = argv[++i];
            if (value.empty() || value.starts_with("--"))
                throw UsageError("Missing value for " + std::string(option));
        }
        result.options.emplace(option, value);
    }
    const auto require = [&](std::string_view option) {
        if (!result.options.contains(option))
            throw UsageError("Missing required option: " + std::string(option));
    };
    if (command == "submit") {
        require("--case");
        require("--profile");
        require("--request-id");
        if (result.options.contains("--timeout-ms") && !result.options.contains("--wait"))
            throw UsageError("submit --timeout-ms requires --wait");
    } else if (command == "get" || command == "wait" || command == "cancel") {
        require("--run-id");
    }
    if (result.options.contains("--timeout-ms"))
        result.timeout = milliseconds(result.options.at("--timeout-ms"));
    if (result.options.contains("--grace-ms"))
        result.grace = milliseconds(result.options.at("--grace-ms"));
    return result;
}

#if CHIPCAE_JOBMGR_LOCAL
// The launcher inherits stdout. Redirect it only while Submit spawns the solver;
// the child keeps stderr as its stdout even after the CLI restores its own fd.
class SolverStdoutGuard {
public:
    SolverStdoutGuard() {
        flush();
        do {
            saved_ = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
        } while (saved_ < 0 && errno == EINTR);
        if (saved_ < 0)
            throw std::system_error(errno, std::generic_category(), "Save stdout");
        if (redirect(STDERR_FILENO) < 0) {
            const auto error = errno;
            close(saved_);
            throw std::system_error(error, std::generic_category(), "Redirect solver stdout");
        }
    }
    ~SolverStdoutGuard() {
        if (saved_ >= 0) {
            redirect(saved_);
            close(saved_);
        }
    }
    SolverStdoutGuard(const SolverStdoutGuard&) = delete;
    SolverStdoutGuard& operator=(const SolverStdoutGuard&) = delete;

    void restore() {
        flush();
        if (redirect(saved_) < 0)
            throw std::system_error(errno, std::generic_category(), "Restore stdout");
        close(saved_);
        saved_ = -1;
    }

private:
    static void flush() {
        std::cout.flush();
        if (!std::cout || std::fflush(stdout) != 0)
            throw std::runtime_error("Flush stdout failed");
    }
    static int redirect(int source) {
        int result;
        do {
            result = dup2(source, STDOUT_FILENO);
        } while (result < 0 && errno == EINTR);
        return result;
    }
    int saved_ = -1;
};

void print(const google::protobuf::Message& message) {
    std::string text;
    if (!google::protobuf::TextFormat::PrintToString(message, &text))
        throw std::runtime_error("Failed to format protobuf response");
    std::cout << text;
}

void dispatch(const Arguments& arguments) {
    namespace jobs = chipcae::jobs::v1;
    chipcae::jobmgr::LocalConfig config;
    config.run_root = std::string(arguments.root);
    const auto& options = arguments.options;
    if (options.contains("--solver"))
        config.solver = std::string(options.at("--solver"));
    if (options.contains("--mpi"))
        config.mpi = std::string(options.at("--mpi"));
    chipcae::jobmgr::LocalJobManager manager(std::move(config));
    const auto command = arguments.command;
    if (command == "submit") {
        jobs::SubmitRequest request;
        request.set_client_request_id(std::string(options.at("--request-id")));
        jobs::Job job;
        {
            SolverStdoutGuard guard;
            job = manager.Submit(
                request, std::string(options.at("--case")), std::string(options.at("--profile")));
            guard.restore();
        }
        if (options.contains("--wait"))
            job = manager.wait(job.run_id(), arguments.timeout);
        print(job);
    } else if (command == "list") {
        print(manager.List(jobs::ListJobsRequest{}));
    } else if (command == "get") {
        jobs::JobRef request;
        request.set_run_id(std::string(options.at("--run-id")));
        print(manager.Get(request));
    } else if (command == "wait") {
        print(manager.wait(std::string(options.at("--run-id")), arguments.timeout));
    } else if (command == "cancel") {
        jobs::CancelRequest request;
        request.set_run_id(std::string(options.at("--run-id")));
        const auto grace = arguments.grace.count();
        request.mutable_grace()->set_seconds(grace / 1000);
        request.mutable_grace()->set_nanos(static_cast<int>((grace % 1000) * 1000000));
        print(manager.Cancel(request));
    } else if (command == "poll") {
        std::cout << "consumed=" << manager.poll() << '\n';
    }
}
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2) {
            const std::string_view option(argv[1]);
            if (option == "--help" || option == "-h") {
                std::cout << usage;
                return 0;
            }
            if (option == "--version" || option == "-v") {
                std::cout << "chipcae-jobmgr " << chipcae::version_string() << '\n';
                return 0;
            }
        }
        const auto arguments = parse(argc, argv);
#if CHIPCAE_JOBMGR_LOCAL
        dispatch(arguments);
        std::cout.flush();
        if (!std::cout)
            throw std::runtime_error("Write stdout failed");
        return 0;
#else
        static_cast<void>(arguments);
        throw std::runtime_error("LocalJobManager not available in this build");
#endif
    } catch (const UsageError& error) {
        std::cerr << error.what() << '\n' << usage;
        return 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
