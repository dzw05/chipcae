if(NOT TARGET chipcae::material)
    message(FATAL_ERROR "cte_contract: chipcae::material target missing")
endif()

chipcae_auto_tests(LIBS chipcae::material LABELS tierA PREFIX ctecontract_)
