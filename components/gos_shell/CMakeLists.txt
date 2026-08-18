# Games link into the shell via the registry. In-development games can be
# absent from a checkout (they are pulled from the public tree until ready):
# any game listed here is included only if its component directory exists,
# and registry.c guards its entry on the matching GOS_HAVE_<NAME> define.
set(GAME_DEPS gunship golf slots)
set(GAME_DEFS "")
foreach(wip bomber)
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/../games/${wip})
        list(APPEND GAME_DEPS ${wip})
        string(TOUPPER ${wip} wip_uc)
        list(APPEND GAME_DEFS GOS_HAVE_${wip_uc})
    endif()
endforeach()

idf_component_register(
    SRCS "shell.c" "registry.c" "apps.c"
    REQUIRES gos_core
    PRIV_REQUIRES gos_hal ${GAME_DEPS} nvs_flash esp_timer
)

foreach(def ${GAME_DEFS})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${def})
endforeach()
