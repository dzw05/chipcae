# Resolve optional third-party packages. Each find is gated by an option that
# defaults to OFF, so a bare toolchain can still configure the tree.

if(CHIPCAE_ENABLE_MPI)
    find_package(MPI REQUIRED COMPONENTS CXX)
endif()

if(CHIPCAE_ENABLE_KOKKOS)
    find_package(Kokkos REQUIRED)
endif()

if(CHIPCAE_ENABLE_HDF5)
    find_package(HDF5 REQUIRED COMPONENTS C)
endif()

if(CHIPCAE_ENABLE_HYPRE)
    find_package(HYPRE REQUIRED)
endif()

if(CHIPCAE_ENABLE_PETSC)
    find_package(PETSc REQUIRED)
endif()

if(CHIPCAE_ENABLE_MFEM)
    find_package(MFEM REQUIRED)
endif()

if(CHIPCAE_ENABLE_JSONCONS)
    find_package(jsoncons REQUIRED)
endif()

if(CHIPCAE_ENABLE_GRPC)
    find_package(Protobuf REQUIRED)
    find_package(gRPC REQUIRED)
endif()

if(CHIPCAE_ENABLE_QT)
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
endif()

if(CHIPCAE_ENABLE_VTK)
    find_package(VTK REQUIRED)
endif()

if(CHIPCAE_ENABLE_OCCT)
    find_package(OpenCASCADE REQUIRED)
endif()

if(CHIPCAE_ENABLE_PYTHON)
    find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
    find_package(pybind11 REQUIRED)
endif()
