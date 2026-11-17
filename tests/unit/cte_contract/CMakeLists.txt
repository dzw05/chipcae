if(NOT TARGET chipcae::material)
    message(FATAL_ERROR "cte_contract: chipcae::material target missing")
endif()

include(canonical_fixtures.cmake)

chipcae_auto_tests(LIBS chipcae::material LABELS tierA PREFIX ctecontract_)
