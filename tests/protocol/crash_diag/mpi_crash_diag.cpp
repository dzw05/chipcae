#include "support.hpp"

#include <memory>
#include <mpi.h>
#include <spawn.h>

extern char** environ;
using namespace crashdiag_test;

namespace {
void collectively_check(bool good, const char* why) {
    const int bad = good ? 0 : 1;
    int any_bad = 0;
    check(MPI_Allreduce(&bad, &any_bad, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD) == MPI_SUCCESS,
          "MPI_Allreduce failed");
    check(any_bad == 0, why);
}
} // namespace

int main(int argc, char** argv) {
    // A fresh exec reaches this branch before MPI_Init; the fatal worker never
    // enters an MPI runtime and no C++ setup runs in a post-MPI fork child.
    if (argc == 3 && std::string_view(argv[1]) == "--signal-worker") {
        try {
            disable_core_dumps();
            CrashDiag::install(argv[2], 1);
            CrashDiag::set_window(4, 1.25);
            ::raise(SIGABRT);
            return 90;
        } catch (...) {
            return 91;
        }
    }
    if (MPI_Init(&argc, &argv) != MPI_SUCCESS)
        return 1;
    int rank = 0;
    int size = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    int result = 0;
    std::unique_ptr<TempDir> owner;
    try {
        collectively_check(size == 2, "acceptance requires exactly two ranks");
        std::string path;
        bool okay = true;
        if (rank == 0) {
            try {
                owner = std::make_unique<TempDir>();
                path = owner->path.string();
            } catch (...) {
                okay = false;
            }
        }
        collectively_check(okay, "rank 0 failed to create shared directory");
        int length = static_cast<int>(path.size());
        MPI_Bcast(&length, 1, MPI_INT, 0, MPI_COMM_WORLD);
        path.resize(static_cast<std::size_t>(length));
        MPI_Bcast(path.data(), length, MPI_CHAR, 0, MPI_COMM_WORLD);
        try {
            CrashDiag::install(path, rank);
            okay = CrashDiag::installed();
        } catch (...) {
            okay = false;
        }
        collectively_check(okay, "both MPI ranks must install crash diagnostics");
        okay = true;
        if (rank == 1) {
            try {
                std::string executable = fs::canonical("/proc/self/exe").string();
                std::array<char*, 4> args{
                    executable.data(), const_cast<char*>("--signal-worker"), path.data(), nullptr};
                pid_t child = -1;
                check(::posix_spawn(
                          &child, executable.c_str(), nullptr, nullptr, args.data(), environ) == 0,
                      "could not spawn pre-MPI signal worker");
                const int status = wait_child(child);
                fatal_status(status, SIGABRT);
                signal_evidence(path, 1, 4, SIGABRT);
            } catch (const std::exception& error) {
                std::cerr << "rank 1 worker: " << error.what() << '\n';
                okay = false;
            }
        }
        collectively_check(okay, "rank 1 signal worker failed acceptance");
        CrashDiag::finalize();
        collectively_check(!CrashDiag::installed(),
                           "MPI finalize left crash diagnostics installed");
        MPI_Barrier(MPI_COMM_WORLD);
        okay = true;
        try {
            signal_evidence(path, 1, 4, SIGABRT);
            check(!fs::exists(diagnostic(path, 0, ".signal")), "rank 0 left signal evidence");
            check(!fs::exists(diagnostic(path, 0, ".json")), "rank 0 left JSON evidence");
        } catch (...) {
            okay = false;
        }
        collectively_check(okay, "MPI rank diagnostics differ from contract");
    } catch (const std::exception& error) {
        std::cerr << "rank " << rank << ": " << error.what() << '\n';
        result = 1;
    }
    CrashDiag::finalize();
    // All acceptance failures above are collective, so both parents still clean
    // up and reach MPI_Finalize instead of hanging or aborting the MPI job.
    MPI_Barrier(MPI_COMM_WORLD);
    owner.reset();
    if (MPI_Finalize() != MPI_SUCCESS)
        result = 1;
    return result;
}
