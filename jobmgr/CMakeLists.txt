add_library(chipcae_jobmgr
    src/job_manager.cpp
    src/local_paths.cpp
)
add_library(chipcae::jobmgr ALIAS chipcae_jobmgr)

target_include_directories(chipcae_jobmgr
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chipcae_jobmgr PUBLIC chipcae::core)
chipcae_set_warnings(chipcae_jobmgr)

add_executable(chipcae-jobmgr src/main.cpp)
target_link_libraries(chipcae-jobmgr PRIVATE chipcae::jobmgr)
chipcae_set_warnings(chipcae-jobmgr)

# TEST-FIRST compile scaffolding for the new acceptance suite. The implementations
# are inert linkage stubs; the existing executable and JobManager stay unchanged.
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
    find_package(Protobuf QUIET)
endif()
if(NOT TARGET jsoncons::jsoncons)
    find_package(jsoncons QUIET)
endif()
if(Protobuf_FOUND AND TARGET jsoncons::jsoncons)
    include(cmake/JobmgrProtocol.cmake)
    chipcae_generate_jobmgr_protocol()
    add_library(chipcae_jobmgr_local src/local_job_manager.cpp)
    add_library(chipcae::jobmgr_local ALIAS chipcae_jobmgr_local)
    target_link_libraries(chipcae_jobmgr_local PUBLIC chipcae::jobmgr chipcae::io
        chipcae_jobmgr_protocol jsoncons::jsoncons)
    chipcae_set_warnings(chipcae_jobmgr_local)
else()
    message(STATUS "jobmgr: LocalJobManager skipped (Protobuf or jsoncons not found)")
endif()
