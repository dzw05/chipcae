chipcae_auto_tests(LIBS chipcae::coupling chipcae::io LABELS tierA PREFIX runctl_ TIMEOUT 60)

if(CHIPCAE_ENABLE_MPI)
    add_executable(chipcae_test_runctl_mpi mpi_run_control.cpp)
    target_link_libraries(chipcae_test_runctl_mpi PRIVATE chipcae::coupling)
    chipcae_set_warnings(chipcae_test_runctl_mpi)
    foreach(ranks 2 4)
        add_test(NAME runctl_mpi_${ranks}
            COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} ${ranks}
                ${MPIEXEC_PREFLAGS} $<TARGET_FILE:chipcae_test_runctl_mpi> ${MPIEXEC_POSTFLAGS})
        set_tests_properties(runctl_mpi_${ranks} PROPERTIES LABELS "mpi;protocol" TIMEOUT 60)
    endforeach()
endif()
