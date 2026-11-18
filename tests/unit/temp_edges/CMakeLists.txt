if(NOT TARGET chipcae::material)
    message(FATAL_ERROR "temp_edges: chipcae::material target missing")
endif()

chipcae_auto_tests(LIBS chipcae::material LABELS tierA PREFIX tempedge_)
