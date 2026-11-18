chipcae_auto_tests(LIBS chipcae::core LABELS tierA PREFIX crashdiag_ TIMEOUT 60)

# Retain automatic discovery and expose the frozen acceptance name as an alias.
add_test(NAME crashdiag_message_escape
    COMMAND chipcae_autotest_tests_protocol_crash_diag_crashdiag_escape)
set_tests_properties(crashdiag_message_escape PROPERTIES LABELS tierA TIMEOUT 60)

if(CHIPCAE_ENABLE_MPI)
    add_executable(chipcae_test_crashdiag_mpi mpi_crash_diag.cpp)
    target_link_libraries(chipcae_test_crashdiag_mpi PRIVATE chipcae::core)
    chipcae_set_warnings(chipcae_test_crashdiag_mpi)
    add_test(NAME crashdiag_mpi_2
        COMMAND ${MPIEXEC_EXECUTABLE} ${MPIEXEC_NUMPROC_FLAG} 2
            ${MPIEXEC_PREFLAGS} $<TARGET_FILE:chipcae_test_crashdiag_mpi> ${MPIEXEC_POSTFLAGS})
    set_tests_properties(crashdiag_mpi_2 PROPERTIES LABELS "mpi;protocol" TIMEOUT 60)
endif()
