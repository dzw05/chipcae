add_library(chipcae_protocol INTERFACE)
add_library(chipcae::protocol ALIAS chipcae_protocol)

target_include_directories(chipcae_protocol INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

if(CHIPCAE_ENABLE_GRPC)
    message(STATUS "gRPC/Protobuf code generation will be wired in a later change")
endif()
