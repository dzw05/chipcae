add_executable(chipcae-preview src/main.cpp)
target_link_libraries(chipcae-preview PRIVATE chipcae::core chipcae::io)
chipcae_set_warnings(chipcae-preview)
