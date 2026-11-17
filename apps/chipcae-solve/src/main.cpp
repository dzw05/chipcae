#include "chipcae/core/error_code.hpp"
#include "chipcae/core/version.hpp"

#include <iostream>
#include <map>
#include <memory>
#ifdef CHIPCAE_SOLVE_MFEM
#include <mfem.hpp>
#endif
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
                         "--out <run_dir> [--cut-output hex]\n";
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--version") {
        if (mpi.rank == 0)
            std::cout << "chipcae-solve " << chipcae::version_string() << '\n';
        return 0;
    }
    std::map<std::string, std::string> args;
    for (int i = 1; i < argc; i += 2) {
        std::string key = argv[i];
        if ((key != "--case" && key != "--profile" && key != "--out" && key != "--cut-output") ||
            i + 1 == argc || args.contains(key) || std::string(argv[i + 1]).starts_with("--") ||
            std::string(argv[i + 1]).empty())
            return fail(chipcae::ErrorCode::INPUT_INVALID,
                        "expected exactly --case <path> --profile <path> --out <path>");
        args.emplace(key, argv[i + 1]);
    }
    if (!args.contains("--case") || !args.contains("--profile") || !args.contains("--out") ||
        (args.contains("--cut-output") && args.at("--cut-output") != "hex"))
        return fail(chipcae::ErrorCode::INPUT_INVALID,
                    "expected exactly --case <path> --profile <path> --out <path>");
#ifdef CHIPCAE_SOLVE_INPUTS
    try {
        auto inputs =
            chipcae::solve::load_inputs(args.at("--case"), args.at("--profile"), mpi.size);
#if defined(MFEM_USE_CUDA) && defined(CHIPCAE_SOLVE_MFEM)
        // MFEM device state is process-global; keep it alive through solve::run below.
        // Device selection alone does not guarantee CUDA support in downstream solver operators.
        std::unique_ptr<mfem::Device> device;
#endif
        if (inputs.backend == "cuda") {
#if !defined(MFEM_USE_CUDA) || !defined(CHIPCAE_SOLVE_MFEM)
            return fail(chipcae::ErrorCode::NOT_IMPLEMENTED,
                        "chipcae-solve built without CUDA-enabled MFEM");
#else
            device = std::make_unique<mfem::Device>("cuda");
            if (mpi.rank == 0)
                device->Print(std::cout);
#endif
        }
#ifdef CHIPCAE_SOLVE_BACKEND
        inputs.cut_as_hex = args.contains("--cut-output");
        return chipcae::solve::run(inputs, args.at("--out"));
#else
        (void)inputs;
#endif
    } catch (const chipcae::solve::InputError& error) {
        return fail(error.code, error.what());
    } catch (const std::exception& error) {
        return fail(1, error.what());
    }
#endif
    return fail(chipcae::ErrorCode::NOT_IMPLEMENTED,
                "numerical backend unavailable: enable JSONcons/MPI/HDF5 and PETSc-enabled MFEM");
}
