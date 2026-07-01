if (DEFINED DSN_CMAKE_INCLUDED)
else()
    set(DSN_ROOT "$ENV{DSN_ROOT}")
    if(NOT EXISTS "${DSN_ROOT}/")
        message(FATAL_ERROR "Please make sure that ${DSN_ROOT} exists.")
    endif()

    include("${DSN_ROOT}/bin/dsn.cmake")
endif()

# The rASN platform is built as a static library that contains the reusable
# agent-system engine (runtime, services, workflow, observability, metrics,
# schema, ...). Both the CodePilot executable and the unit-test executable link
# this library instead of recompiling every engine source. Application entry
# points live with their executables (apps/*/main.cpp), so this directory
# intentionally contains no main().
set(MY_PROJ_NAME rasn)

# Engine source files under THIS directory are automatically included.
set(MY_PROJ_SRC "")

set(MY_SRC_SEARCH_MODE "GLOB")

set(MY_PROJ_INC_PATH "")

set(MY_PROJ_LIBS "")

set(MY_PROJ_LIB_PATH "")

set(MY_BINPLACES "")

dsn_add_static_library()

add_subdirectory(apps/codepilot)
add_subdirectory(tests)
