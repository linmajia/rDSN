if (DEFINED DSN_CMAKE_INCLUDED)
else()
    set(DSN_ROOT "$ENV{DSN_ROOT}")
    if(NOT EXISTS "${DSN_ROOT}/")
        message(FATAL_ERROR "Please make sure that ${DSN_ROOT} exists.")
    endif()

    include("${DSN_ROOT}/bin/dsn.cmake")
endif()

set(MY_PROJ_NAME codepilot)

# Source files under CURRENT project directory will be automatically included.
set(MY_PROJ_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/codepilot/codepilot_app.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/codepilot/local_tools.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/codepilot/skills.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/redaction.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/schema_manifest.cpp")

set(MY_SRC_SEARCH_MODE "GLOB")

set(MY_PROJ_INC_PATH "")

if (UNIX)
    set(MY_PROJ_LIBS pthread)
endif()

set(MY_PROJ_LIB_PATH "")

set(MY_BINPLACES "${CMAKE_CURRENT_SOURCE_DIR}/config.ini")

dsn_add_executable()

add_subdirectory(tests)
