if (DEFINED DSN_CMAKE_INCLUDED)
else()
    set(DSN_ROOT "$ENV{DSN_ROOT}")
    if(NOT EXISTS "${DSN_ROOT}/")
        message(FATAL_ERROR "Please make sure that ${DSN_ROOT} exists.")
    endif()

    include("${DSN_ROOT}/bin/dsn.cmake")
endif()

# CodePilot is the first rASN application under apps/. It is a thin executable:
# the agent engine comes from the rasn static library, and this directory only
# contributes the CLI/service front-end (codepilot_app), built-in local tools,
# skill prompts, and the process entry point (main.cpp).
set(MY_PROJ_NAME codepilot)

# Source files under THIS directory (codepilot_app.cpp, local_tools.cpp,
# skills.cpp, main.cpp) are automatically included.
set(MY_PROJ_SRC "")

set(MY_SRC_SEARCH_MODE "GLOB")

# Allow app sources to include reusable rASN headers as `<rasn/<header>.h>`.
set(MY_PROJ_INC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# Only the rasn static library is needed here. rasn re-exports its own link
# requirements: when the rDSN.dist.service plugin is in the build, rasn's
# coordination backend pulls its distributed_lock_service / meta_state_service
# provider closure in as a PUBLIC dependency, so it propagates here transitively
# and this executable does not repeat the list. rDSN also links the platform
# thread library into every executable (CMAKE_THREAD_LIBS_INIT, carried in
# DSN_SYSTEM_LIBS) and adds -pthread globally on UNIX, and it auto-appends
# dsn.dev.cpp/dsn.core/dsn.dev.utility + boost. So we do not list pthread or the
# rDSN core libs explicitly.
set(MY_PROJ_LIBS rasn)

set(MY_PROJ_LIB_PATH "")

# Binplace this app's own thin config.ini (the app always loads it) alongside the
# runtime host/config defaults and the optional replicated-state cluster profile.
# A plain `./codepilot <cmd>` loads ONLY config.ini (thin, local placement -- no
# service/deployment config is ever exposed to the app). `./codepilot serve` loads
# the runtime host config.rasn.ini (full service fleet), which @includes
# config.rasn.defaults.ini for shared module tuning. config.rasn.defaults.ini is
# also what a local app may optionally @include to tune its in-process runtime.
set(MY_BINPLACES
    "${CMAKE_CURRENT_SOURCE_DIR}/config.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../config.rasn.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../config.rasn.state.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../config.rasn.runtime.replicated.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../config.rasn.defaults.ini")

set(RASN_INTEGRATION_PROGRAMS
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/integration/run_multinode.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/integration/test.sh")

dsn_add_executable()

# MY_BINPLACES uses install(FILES), which removes execute permissions. Keep the
# integration entry points executable in both build and install trees. A separate
# dependency target also refreshes them when only a script changes and CodePilot
# itself does not need relinking.
add_custom_target(
    ${MY_PROJ_NAME}_integration_programs
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:${MY_PROJ_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/integration/run_multinode.py"
            "$<TARGET_FILE_DIR:${MY_PROJ_NAME}>/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/integration/test.sh"
            "$<TARGET_FILE_DIR:${MY_PROJ_NAME}>/"
    DEPENDS ${RASN_INTEGRATION_PROGRAMS}
    VERBATIM)
add_dependencies(${MY_PROJ_NAME} ${MY_PROJ_NAME}_integration_programs)

if(NOT DSN_BUILD_RUNTIME)
    foreach(RASN_INTEGRATION_PROGRAM ${RASN_INTEGRATION_PROGRAMS})
        install(PROGRAMS "${RASN_INTEGRATION_PROGRAM}"
                DESTINATION "bin/${MY_PROJ_NAME}")
    endforeach()
endif()
