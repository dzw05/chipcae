add_executable(sp4_step_fixtures step_fixtures.cpp)
target_include_directories(sp4_step_fixtures SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(sp4_step_fixtures PRIVATE TKPrim TKBool TKSTEP TKXDESTEP TKXCAF)
set_property(TARGET sp4_step_fixtures PROPERTY CXX_STANDARD 17)
