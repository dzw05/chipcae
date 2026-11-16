option(CHIPCAE_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

function(chipcae_set_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4>
        $<$<AND:$<BOOL:${CHIPCAE_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:GNU,Clang,AppleClang>>:-Werror>
        $<$<AND:$<BOOL:${CHIPCAE_WARNINGS_AS_ERRORS}>,$<CXX_COMPILER_ID:MSVC>>:/WX>
    )
endfunction()
