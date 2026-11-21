#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if CHIPCAE_JOBMGR_LOCAL
#include "chipcae/jobmgr/local_paths.hpp"
#include "chipcae/jobs/v1/jobs.pb.h"

#include <google/protobuf/text_format.h>
#endif

namespace jobmgr_cli_test {
namespace fs = std::filesystem;

inline void require(bool condition, std::string_view why) {
    if (!condition)
        throw std::runtime_error(std::string(why));
}

inline std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}

inline void write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(!output.fail(), "cannot write " + path.string());
}

class TempDirectory {
public:
    TempDirectory() {
        const auto pattern = (fs::temp_directory_path() / "chipcae cli XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        const auto* created = mkdtemp(buffer.data());
        require(created != nullptr, "mkdtemp failed");
        path = created;
    }
    ~TempDirectory() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;
    fs::path path;
};

struct Result {
    int code;
    std::string out;
    std::string err;
    std::shared_ptr<TempDirectory> capture;

    Result refresh() const {
        return {code, read(capture->path / "stdout"), read(capture->path / "stderr"), capture};
    }
};

// Capture files avoid pipe-buffer deadlocks; argv is never interpreted by a shell.
inline Result cli(const std::vector<std::string>& arguments) {
    auto capture = std::make_shared<TempDirectory>();
    const auto stdout_path = (capture->path / "stdout").string();
    const auto stderr_path = (capture->path / "stderr").string();
    std::vector<std::string> storage{CHIPCAE_JOBMGR_EXE};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : storage)
        argv.push_back(argument.data());
    argv.push_back(nullptr);
    const auto child = fork();
    require(child >= 0, "fork failed");
    if (child == 0) {
        const int output = open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int error = open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (output < 0 || error < 0 || dup2(output, STDOUT_FILENO) < 0 ||
            dup2(error, STDERR_FILENO) < 0)
            _exit(126);
        close(output);
        close(error);
        execv(argv.front(), argv.data());
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    require(waited == child, "waitpid failed");
    require(WIFEXITED(status), "CLI terminated by signal; status=" + std::to_string(status));
    Result result{WEXITSTATUS(status), read(stdout_path), read(stderr_path), std::move(capture)};
    require(result.code != 126 && result.code != 127, "CLI capture/exec failed: " + result.err);
    std::cout << "CLI";
    for (const auto& argument : arguments)
        std::cout << " [" << argument << ']';
    std::cout << " => exit=" << result.code << '\n';
    return result;
}

inline void exit_code(const Result& result, int expected) {
    require(result.code == expected,
            "expected exit=" + std::to_string(expected) + ", got=" + std::to_string(result.code) +
                " stdout={" + result.out + "} stderr={" + result.err + "}");
    require(result.out.find("chipcae-jobmgr is idle") == std::string::npos &&
                result.err.find("chipcae-jobmgr is idle") == std::string::npos,
            "obsolete idle output: stdout={" + result.out + "} stderr={" + result.err + "}");
}

inline void usage_error(const std::vector<std::string>& arguments) {
    const auto result = cli(arguments);
    exit_code(result, 2);
    std::string error = result.err;
    std::transform(error.begin(), error.end(), error.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    require(error.find("usage:") != std::string::npos, "stderr missing usage: " + result.err);
    require(error.find('\n') != std::string::npos && error.find("usage:") > 0,
            "stderr must include an error before usage: " + result.err);
}

class Suite {
public:
    template <class F>
    void test(std::string_view name, F&& body) {
        try {
            body();
            std::cout << "PASS: " << name << '\n';
        } catch (const std::exception& error) {
            ++failures_;
            std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        }
    }
    int result() const { return failures_ == 0 ? EXIT_SUCCESS : EXIT_FAILURE; }

private:
    int failures_ = 0;
};

#if CHIPCAE_JOBMGR_LOCAL
namespace jobs = chipcae::jobs::v1;
inline constexpr std::string_view request_id = "12345678-1234-4ABC-8DEF-123456789ABC";
inline constexpr std::string_view unknown_id = "00000000-0000-4000-8000-000000000099";

template <class Message>
Message text_message(const Result& result) {
    exit_code(result, 0);
    Message message;
    require(google::protobuf::TextFormat::ParseFromString(result.out, &message),
            "entire stdout must parse as protobuf text: {" + result.out + "}");
    return message;
}

struct Fixture {
    TempDirectory temp;
    fs::path root = temp.path / "run root with spaces";
    fs::path source = temp.path / "synthetic inputs";
    fs::path case_path = source / "case with spaces.json";
    fs::path profile = source / "profile with spaces.json";
    fs::path solver = temp.path / "solver with spaces";

    Fixture() {
        fs::create_directories(root);
        fs::create_symlink(CHIPCAE_JOBMGR_MOCK, solver);
        write(case_path, R"({
            "schema":"chipcae.case/1.0",
            "stack":{"ref":"stack.json"},"materials":{"ref":"materials.json"},
            "mesh":{"type":"aligned_cartesian_cut","nx":2,"ny":2,
                    "cut":{"stabilization":"ghost_penalty"}},
            "physics":[{"id":"mech","solver":"fem.solid"}],
            "loads":[{"type":"uniform_temperature","T_ref":298.15,"T_target":308.15}],
            "constraints":{"rigid":"3-2-1"},
            "output":{"format":"vtkhdf","warpage":{
                "standard":"JESD22-B112","remove_rigid":"best_fit_plane"}}
        })");
        write(source / "stack.json", R"({
            "id":"synthetic-cli","unit":"m","outline":{"box":[0,0,0.01,0.01]},
            "symmetry":"none","package_type":"GENERIC",
            "layers":[{"name":"layer","thickness":0.001,"kind":"Homogenized",
                       "material":"synthetic","n_elem_min":2,"grading":1}]
        })");
        write(source / "materials.json", R"({
            "schema":"chipcae.material_library/1","materials":[{
                "id":"synthetic","schema_version":"1","unit_system":"SI",
                "density":1000,"conductivity":1,"model":"thermo_elastic",
                "provenance":{"source_type":"illustrative","confidence":"illustrative",
                              "notes":"Synthetic CLI acceptance material"},
                "elastic":{"symmetry":"isotropic","table":{
                    "temperatures":[290,310],"columns":{"E":[1e9,1e9],"nu":[0.3,0.3]},
                    "interpolation":"linear","extrapolation":"clamp"}},
                "thermal_expansion":{"cte_type":"tangent","symmetry":"isotropic",
                    "stress_free_temperature":298.15,"table":{
                        "temperatures":[290,310],"columns":{"alpha":[1e-5,1e-5]},
                        "interpolation":"linear","extrapolation":"clamp"}}
            }]})");
        write(profile, R"({"schema":"chipcae.run_profile/1.0","ranks":1,
            "backend":"cpu","gpus":0,"threads_per_rank":1,"launcher":"direct"})");
    }

    std::vector<std::string> submit_args(std::string_view id = request_id) const {
        return {"--root",
                root.string(),
                "submit",
                "--case",
                case_path.string(),
                "--profile",
                profile.string(),
                "--request-id",
                std::string(id),
                "--solver",
                solver.string()};
    }
    Result command(const std::vector<std::string>& tail) const {
        std::vector<std::string> arguments{"--root", root.string()};
        arguments.insert(arguments.end(), tail.begin(), tail.end());
        return cli(arguments);
    }
    jobs::Job submit() const {
        const auto job = text_message<jobs::Job>(cli(submit_args()));
        require(chipcae::jobmgr::is_uuid(job.run_id()), "submit run_id must be a UUID");
        require(job.client_request_id() == request_id, "request-id must be preserved exactly");
        return job;
    }
};

inline void completed(const jobs::Job& job, std::string_view run_id) {
    require(job.run_id() == run_id, "wait returned a different run_id");
    std::cout << "Job state=" << jobs::JobState_Name(job.state())
              << " completion=" << jobs::CompletionKind_Name(job.completion()) << '\n';
    require(job.state() == jobs::JOB_STATE_COMPLETED,
            "expected JOB_STATE_COMPLETED, got " + jobs::JobState_Name(job.state()) + ": " +
                job.failure().message());
    require(job.completion() == jobs::COMPLETION_KIND_END_REACHED,
            "expected COMPLETION_KIND_END_REACHED");
    require(!job.has_failure(), "successful mock must have no failure");
}
#endif
} // namespace jobmgr_cli_test
