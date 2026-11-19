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
template <class T, PetscErrorCode (*Destroy)(T*)>
struct Handle {
    T value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (value)
            Destroy(&value);
    }
};
using Matrix = Handle<Mat, MatDestroy>;
using Vector = Handle<Vec, VecDestroy>;
using Krylov = Handle<KSP, KSPDestroy>;
using Scatter = Handle<VecScatter, VecScatterDestroy>;
} // namespace chipcae::solvers::fvm::detail
