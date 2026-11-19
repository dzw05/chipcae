#pragma once
#include <limits>
#include <petscksp.h>
#include <stdexcept>
#include <string>
namespace chipcae::solvers::fvm::detail {
inline PetscInt index(std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<PetscInt>::max()))
        throw std::runtime_error("INPUT_INVALID: PETSc index overflow");
    return static_cast<PetscInt>(n);
}
inline void collective_check(PetscErrorCode code, MPI_Comm comm, const char* operation) {
    int failed = code ? 1 : 0, any = 0;
    if (MPI_Allreduce(&failed, &any, 1, MPI_INT, MPI_MAX, comm) != MPI_SUCCESS)
        throw std::runtime_error("INTERNAL: MPI failure collecting PETSc status");
    if (any)
        throw std::runtime_error(std::string("INTERNAL: PETSc ") + operation + " failed");
}
struct Matrix {
    Mat value = nullptr;
    ~Matrix() {
        if (value)
            MatDestroy(&value);
    }
};
struct Vector {
    Vec value = nullptr;
    ~Vector() {
        if (value)
            VecDestroy(&value);
    }
};
struct Krylov {
    KSP value = nullptr;
    ~Krylov() {
        if (value)
            KSPDestroy(&value);
    }
};
struct Scatter {
    VecScatter value = nullptr;
    ~Scatter() {
        if (value)
            VecScatterDestroy(&value);
    }
};
} // namespace chipcae::solvers::fvm::detail
