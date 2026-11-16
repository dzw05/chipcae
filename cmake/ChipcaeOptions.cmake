# Build and dependency options. Optional third-party packages default to OFF so
# the skeleton configures and compiles with a plain C++20 toolchain.

option(CHIPCAE_BUILD_TESTS "Build unit and integration tests" ON)
option(CHIPCAE_BUILD_GUI "Build the Qt6 desktop client" OFF)
option(CHIPCAE_ENABLE_PYTHON "Build Python bindings" OFF)

option(CHIPCAE_ENABLE_KOKKOS "Use Kokkos for on-node parallelism" OFF)
option(CHIPCAE_ENABLE_MPI "Use MPI for distributed parallelism" OFF)
option(CHIPCAE_ENABLE_HDF5 "Enable HDF5 / VTKHDF I/O" OFF)
option(CHIPCAE_ENABLE_HYPRE "Enable Hypre linear solvers" OFF)
option(CHIPCAE_ENABLE_PETSC "Enable PETSc linear solvers" OFF)
option(CHIPCAE_ENABLE_JSONCONS "Enable jsoncons for JSON I/O" OFF)
option(CHIPCAE_ENABLE_GRPC "Enable gRPC and protobuf code generation" OFF)
option(CHIPCAE_ENABLE_MFEM "Enable MFEM (conditional; requires prototype P1)" OFF)
option(CHIPCAE_ENABLE_QT "Enable Qt6 (implied by CHIPCAE_BUILD_GUI)" OFF)
option(CHIPCAE_ENABLE_VTK "Enable VTK visualization" OFF)
option(CHIPCAE_ENABLE_OCCT "Enable Open CASCADE (OCCT) CAD import" OFF)

if(CHIPCAE_BUILD_GUI)
    set(CHIPCAE_ENABLE_QT ON CACHE BOOL "Enable Qt6 (implied by CHIPCAE_BUILD_GUI)" FORCE)
endif()
