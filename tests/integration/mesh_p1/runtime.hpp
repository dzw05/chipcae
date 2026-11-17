#pragma once
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif
#if __has_include(<mfem.hpp>)
#include <mfem.hpp>
#define K1_HAS_MFEM 1
#endif
namespace k1 {
// MPI tests must initialize hypre before the first HypreParMatrix is built
// (mfem/mfem#4943). Production solvers should call the same pair.
inline void init_runtime([[maybe_unused]] int& argc, [[maybe_unused]] char** argv) {
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Init(&argc, &argv);
#endif
#ifdef K1_HAS_MFEM
    mfem::Hypre::Init();
#endif
}
inline void finalize_runtime() {
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Finalize();
#endif
}
} // namespace k1
