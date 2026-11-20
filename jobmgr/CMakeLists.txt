add_library(chipcae_jobmgr
    src/sha256.cpp
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
    add_library(chipcae_jobmgr_local
        src/local_job_manager.cpp
        src/posix_spawner.cpp
        src/run_dir.cpp
        src/profile_map.cpp
        src/outcome.cpp
        src/launcher_lock.cpp)
    target_compile_definitions(chipcae_jobmgr_local
        PRIVATE CHIPCAE_SCHEMA_DIR="${PROJECT_SOURCE_DIR}/schema")
    add_library(chipcae::jobmgr_local ALIAS chipcae_jobmgr_local)
    target_link_libraries(chipcae_jobmgr_local PUBLIC chipcae::jobmgr chipcae::io
        chipcae_jobmgr_protocol jsoncons::jsoncons)
    # Use the compiler's POSIX thread support; no additional package dependency.
    target_compile_options(chipcae_jobmgr_local PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-pthread>)
    target_link_options(chipcae_jobmgr_local PUBLIC
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-pthread>)
    chipcae_set_warnings(chipcae_jobmgr_local)
else()
    message(STATUS "jobmgr: LocalJobManager skipped (Protobuf or jsoncons not found)")
endif()
