add_library(chipcae_io
    src/manifest.cpp
    src/vtkhdf_results.cpp
    src/checkpoint.cpp
    src/result_writer.cpp
    src/event_log.cpp
)
add_library(chipcae::io ALIAS chipcae_io)

target_include_directories(chipcae_io
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chipcae_io PUBLIC chipcae::core)
chipcae_set_warnings(chipcae_io)

if(CHIPCAE_ENABLE_HDF5)
    target_link_libraries(chipcae_io PUBLIC HDF5::HDF5)
    target_compile_definitions(chipcae_io PUBLIC CHIPCAE_ENABLE_HDF5=1)
endif()

if(CHIPCAE_ENABLE_HDF5)
    add_executable(chipcae_write_results ${PROJECT_SOURCE_DIR}/tools/samples/write_results.cpp)
    target_link_libraries(chipcae_write_results PRIVATE chipcae::io)
    chipcae_set_warnings(chipcae_write_results)
endif()
target_sources(chipcae_io PRIVATE src/control_file.cpp)

if(CHIPCAE_RESULTS_FAULT_INJECTION AND CHIPCAE_BUILD_TESTS)
    target_compile_definitions(chipcae_io PRIVATE CHIPCAE_RESULTS_FAULT_INJECTION=1)
endif()
