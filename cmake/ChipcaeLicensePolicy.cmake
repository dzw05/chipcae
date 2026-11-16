# License policy for the closed-source commercial product (ADR-0001 / D1).
#
# Allowed third-party dependencies (permissive or LGPL with dynamic linking):
#   Kokkos, MPI implementations, HDF5, Hypre, PETSc, jsoncons, gRPC/Protobuf,
#   MFEM (conditional), Qt6, VTK, OCCT.
#
# Excluded:
#   GPL libraries such as p4est, t8code and Basilisk.
#   AMReX is not a project dependency (see ADR-0002).
#   deal.II is a backup only and must not be added until a non-p4est path is confirmed.

set(_chipcae_license_script "${CMAKE_SOURCE_DIR}/cmake/check_license_policy.py")
if(EXISTS "${_chipcae_license_script}")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" "${_chipcae_license_script}" "${CMAKE_SOURCE_DIR}"
            RESULT_VARIABLE _chipcae_license_result
            OUTPUT_VARIABLE _chipcae_license_output
            ERROR_VARIABLE _chipcae_license_output
        )
        if(NOT _chipcae_license_result EQUAL 0)
            message(FATAL_ERROR "License policy check failed:\n${_chipcae_license_output}")
        endif()
    else()
        message(STATUS "Python3 not found; skipping configure-time license policy scan")
    endif()
endif()
