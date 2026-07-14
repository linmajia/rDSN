if (DEFINED DSN_CMAKE_INCLUDED)
else()
    set(DSN_ROOT "$ENV{DSN_ROOT}")
    if(NOT EXISTS "${DSN_ROOT}/")
        message(FATAL_ERROR "Please make sure that ${DSN_ROOT} exists.")
    endif()

    include("${DSN_ROOT}/bin/dsn.cmake")
endif()

# SREPilot is the second rASN application under apps/. The executable is a thin
# incident-response front-end over the reusable rasn static library.
set(MY_PROJ_NAME srepilot)

set(MY_PROJ_SRC "")

set(MY_SRC_SEARCH_MODE "GLOB")

set(MY_PROJ_INC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# Only the rasn static library is needed here. rasn re-exports its coordination
# provider closure (distributed_lock_service / meta_state_service) as a PUBLIC
# link dependency when the rDSN.dist.service plugin is in the build, so it
# propagates here transitively and this executable does not repeat the list.
set(MY_PROJ_LIBS rasn)

set(MY_PROJ_LIB_PATH "")

# Binplace this app's own thin config.ini (the app always loads it) alongside the
# runtime host config.rasn.ini and the shared tuning defaults config.rasn.defaults.ini.
# A plain `./srepilot <cmd>` loads ONLY config.ini (thin, local placement -- no
# service/deployment config is ever exposed to the app). `./srepilot serve` loads
# the runtime host config.rasn.ini (full service fleet), which @includes
# config.rasn.defaults.ini for shared module tuning. config.rasn.defaults.ini is
# also what a local app may optionally @include to tune its in-process runtime.
set(MY_BINPLACES
    "${CMAKE_CURRENT_SOURCE_DIR}/config.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../config.rasn.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../config.rasn.state.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../config.rasn.runtime.replicated.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../config.rasn.defaults.ini")

dsn_add_executable()
