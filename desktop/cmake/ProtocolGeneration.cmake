# Public target chipcae_desktop_protocol, headers relative to its generated include root.
function(chipcae_generate_protocol)
    if(TARGET chipcae_desktop_protocol)
        return()
    endif()
    get_filename_component(protocol_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../protocol" ABSOLUTE)
    get_target_property(protoc protobuf::protoc IMPORTED_LOCATION_RELEASE)
    if(NOT protoc)
        get_target_property(protoc protobuf::protoc IMPORTED_LOCATION)
    endif()
    execute_process(COMMAND "${protoc}" --version OUTPUT_VARIABLE protoc_version
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    # Modern protobuf C++ 6.x/7.x corresponds to protoc 30.x/35.x; lock the SDK pair
    # by the protobuf package's own exported protoc target and recorded binary hash.
    file(SHA256 "${protoc}" protoc_hash)
    message(STATUS "Protocol generator: ${protoc_version}, sha256=${protoc_hash}; runtime=${Protobuf_VERSION}")
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
    foreach(dir IN LISTS protobuf_includes)
        if(EXISTS "${dir}/google/protobuf/timestamp.proto")
            list(APPEND import_args "--proto_path=${dir}")
            set(well_known "${dir}/google/protobuf/timestamp.proto" "${dir}/google/protobuf/duration.proto")
        endif()
    endforeach()
    add_custom_command(OUTPUT ${outputs}
        COMMAND protobuf::protoc ${import_args} "--cpp_out=${generated}" ${protos}
        WORKING_DIRECTORY "${protocol_root}"
        DEPENDS protobuf::protoc "${protocol_root}/chipcae/run/v1/run.proto"
            "${protocol_root}/chipcae/jobs/v1/jobs.proto" ${well_known}
        VERBATIM)
    add_library(chipcae_desktop_protocol STATIC ${outputs})
    target_include_directories(chipcae_desktop_protocol PUBLIC "${generated}")
    target_link_libraries(chipcae_desktop_protocol PUBLIC protobuf::libprotobuf)
    set_target_properties(chipcae_desktop_protocol PROPERTIES AUTOMOC OFF)
endfunction()
