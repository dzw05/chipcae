# Compile the frozen messages so acceptance tests use the actual protobuf types.
# No gRPC generation or dependency on desktop targets.
function(chipcae_generate_jobmgr_protocol)
    set(protocol_root "${PROJECT_SOURCE_DIR}/protocol")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${generated}")
    set(protos chipcae/run/v1/run.proto chipcae/jobs/v1/jobs.proto)
    set(outputs)
    foreach(proto IN LISTS protos)
        string(REPLACE ".proto" ".pb.cc" cc "${generated}/${proto}")
        string(REPLACE ".proto" ".pb.h" hh "${generated}/${proto}")
        list(APPEND outputs "${cc}" "${hh}")
    endforeach()
    get_target_property(protobuf_includes protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
    set(import_args "--proto_path=${protocol_root}")
    set(well_known)
    foreach(dir IN LISTS protobuf_includes)
        if(EXISTS "${dir}/google/protobuf/timestamp.proto")
            list(APPEND import_args "--proto_path=${dir}")
            list(APPEND well_known "${dir}/google/protobuf/timestamp.proto"
                "${dir}/google/protobuf/duration.proto")
        endif()
    endforeach()
    add_custom_command(OUTPUT ${outputs}
        COMMAND protobuf::protoc ${import_args} "--cpp_out=${generated}" ${protos}
        WORKING_DIRECTORY "${protocol_root}"
        DEPENDS protobuf::protoc "${protocol_root}/chipcae/run/v1/run.proto"
            "${protocol_root}/chipcae/jobs/v1/jobs.proto" ${well_known}
        VERBATIM)
    add_library(chipcae_jobmgr_protocol STATIC ${outputs})
    target_include_directories(chipcae_jobmgr_protocol PUBLIC "${generated}")
    target_link_libraries(chipcae_jobmgr_protocol PUBLIC protobuf::libprotobuf)
endfunction()
