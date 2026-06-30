if (DEFINED DSN_CMAKE_INCLUDED)
else()
    set(DSN_ROOT "$ENV{DSN_ROOT}")
    if(NOT EXISTS "${DSN_ROOT}/")
        message(FATAL_ERROR "Please make sure that ${DSN_ROOT} exists.")
    endif()

    include("${DSN_ROOT}/bin/dsn.cmake")
endif()

# CodePilot is the first rASN application. It is a thin executable: the agent
# engine comes from the rasn static library, and this directory only contributes
# the CLI/service front-end (codepilot_app), built-in local tools, skill prompts,
# and the process entry point (main.cpp).
set(MY_PROJ_NAME codepilot)

# Source files under THIS directory (codepilot_app.cpp, local_tools.cpp,
# skills.cpp, main.cpp) are automatically included.
set(MY_PROJ_SRC "")

set(MY_SRC_SEARCH_MODE "GLOB")

# Allow `#include "../<engine_header>.h"` and `#include "<engine_header>.h"` to
# resolve against the engine sources in the parent directory.
set(MY_PROJ_INC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Only the rasn static library is needed here. rDSN already links the platform
# thread library into every executable (CMAKE_THREAD_LIBS_INIT, carried in
# DSN_SYSTEM_LIBS) and adds -pthread globally on UNIX, and it auto-appends
# dsn.dev.cpp/dsn.core/dsn.dev.utility + boost. So we do not list pthread or the
# rDSN core libs explicitly.
set(MY_PROJ_LIBS rasn)

set(MY_PROJ_LIB_PATH "")

set(MY_BINPLACES "${CMAKE_CURRENT_SOURCE_DIR}/../config.ini")

dsn_add_executable()
