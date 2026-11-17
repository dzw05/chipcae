add_executable(chipcae-preview src/main.cpp)
target_link_libraries(chipcae-preview PRIVATE chipcae::core chipcae::io)
chipcae_set_warnings(chipcae-preview)
if(CHIPCAE_ENABLE_JSONCONS AND CHIPCAE_ENABLE_HDF5)
  target_link_libraries(chipcae-preview PRIVATE chipcae_stack chipcae_mesh_background chipcae_mesh_vtkhdf)
  target_compile_definitions(chipcae-preview PRIVATE
    CHIPCAE_PREVIEW_ENABLED=1
    CHIPCAE_PREVIEW_SCHEMA="${PROJECT_SOURCE_DIR}/schema/stack-0.2.schema.json")
endif()
