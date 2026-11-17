#include "chipcae/core/error_code.hpp"

#include <iostream>
#include <map>
#include <string>
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif
#ifdef CHIPCAE_SOLVE_INPUTS
#include "chipcae/solve/case_loader.hpp"
#endif
namespace {
struct MpiSession {
    int rank = 0, size = 1;
    MpiSession(int& argc, char**& argv) {
#ifdef CHIPCAE_ENABLE_MPI
        MPI_Init(&argc, &argv);
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        MPI_Comm_size(MPI_COMM_WORLD, &size);
#else
        (void)argc;
        (void)argv;
#endif
    }
    ~MpiSession() {
#ifdef CHIPCAE_ENABLE_MPI
        MPI_Finalize();
#endif
    }
};
} // namespace
int main(int argc, char** argv) {
    MpiSession mpi(argc, argv);
    auto fail = [&](int code, const std::string& message) {
        if (mpi.rank == 0)
            std::cerr << "chipcae-solve: " << message << '\n';
        return code;
    };
    if (argc == 2 && std::string(argv[1]) == "--help") {
        if (mpi.rank == 0)
            std::cout << "Usage: chipcae-solve --case <case.json> --profile <run_profile.json> "
                         "--out <run_dir>\n";
        return 0;
    }
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; i += 2) {
        std::string key = argv[i];
        if ((key != "--case" && key != "--profile" && key != "--out") || i + 1 == argc ||
            args.contains(key) || std::string(argv[i + 1]).starts_with("--") ||
            std::string(argv[i + 1]).empty())
            return fail(chipcae::ErrorCode::INPUT_INVALID,
                        "expected exactly --case <path> --profile <path> --out <path>");
        args.emplace(key, argv[i + 1]);
    }
    if (args.size() != 3)
        return fail(chipcae::ErrorCode::INPUT_INVALID,
                    "expected exactly --case <path> --profile <path> --out <path>");
#ifdef CHIPCAE_SOLVE_INPUTS
    try {
        const auto inputs =
            chipcae::solve::load_inputs(args.at("--case"), args.at("--profile"), mpi.size);
        (void)inputs;
    } catch (const chipcae::solve::InputError& error) {
        return fail(error.code, error.what());
    }
#endif
    return fail(chipcae::ErrorCode::NOT_IMPLEMENTED,
                "O2 numerical backend not implemented: requires MFEM/PETSc/MPI/HDF5 and reviewed "
                "material conductivity/constraint interfaces");
}
