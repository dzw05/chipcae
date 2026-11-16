add_library(chipcae_jobmgr
    src/job_manager.cpp
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
