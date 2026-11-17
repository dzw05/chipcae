if(NOT TARGET chipcae::material)
    message(STATUS "Skipping temp_edges: chipcae::material is unavailable (requires Kokkos)")
    return()
endif()

chipcae_auto_tests(LIBS chipcae::material LABELS tierA PREFIX tempedge_)
