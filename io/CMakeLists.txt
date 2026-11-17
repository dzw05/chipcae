add_library(chipcae_io
    src/manifest.cpp
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

target_sources(chipcae_io PRIVATE src/control_file.cpp)
