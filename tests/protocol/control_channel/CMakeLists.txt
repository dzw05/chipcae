chipcae_auto_tests(LIBS chipcae::coupling chipcae::io LABELS tierA PREFIX ctlch_ TIMEOUT 60)

if(CHIPCAE_ENABLE_MPI)
    add_executable(chipcae_test_ctlch_mpi mpi_control_channel.cpp)
    target_link_libraries(chipcae_test_ctlch_mpi PRIVATE chipcae::coupling chipcae::io)
    chipcae_set_warnings(chipcae_test_ctlch_mpi)
    foreach(ranks 2 4)
        add_test(NAME ctlch_mpi_${ranks}
            COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} ${ranks}
                ${MPIEXEC_PREFLAGS} $<TARGET_FILE:chipcae_test_ctlch_mpi> ${MPIEXEC_POSTFLAGS})
        set_tests_properties(ctlch_mpi_${ranks} PROPERTIES LABELS "mpi;protocol" TIMEOUT 60)
    endforeach()
endif()
