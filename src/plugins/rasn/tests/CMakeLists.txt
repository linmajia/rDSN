if (DEFINED DSN_CMAKE_INCLUDED)
else()
    set(DSN_ROOT "$ENV{DSN_ROOT}")
    if(NOT EXISTS "${DSN_ROOT}/")
        message(FATAL_ERROR "Please make sure that ${DSN_ROOT} exists.")
    endif()

    include("${DSN_ROOT}/bin/dsn.cmake")
endif()

# The rASN engine is linked from the rasn static library, so the unit-test
# executable only compiles the test sources plus the CodePilot local-tools
# component it exercises directly (apps/codepilot/local_tools.cpp, which is part
# of the CodePilot executable rather than the engine library).
set(RASN_UNIT_TEST_SUPPORT_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/../apps/codepilot/local_tools.cpp")

set(MY_PROJ_NAME rasn.unit_tests)

set(MY_PROJ_SRC
    ${RASN_UNIT_TEST_SUPPORT_SRC}
    "${CMAKE_CURRENT_SOURCE_DIR}/standalone/rasn_unit_test_main.cpp")

set(MY_SRC_SEARCH_MODE "GLOB")

set(MY_PROJ_INC_PATH
    ${GTEST_INCLUDE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/..")

# gtest plus the rasn static library. The platform thread library that gtest
# needs is linked automatically by rDSN (CMAKE_THREAD_LIBS_INIT, carried in
# DSN_SYSTEM_LIBS and appended after these libs), so pthread is not listed here.
set(MY_PROJ_LIBS gtest rasn)

set(MY_PROJ_LIB_PATH "")

if (WIN32)
    set(MY_BINPLACES "${CMAKE_CURRENT_SOURCE_DIR}/standalone/test.cmd")
else()
    set(MY_BINPLACES "")
endif()

dsn_add_executable()

if (UNIX)
    add_custom_command(
        TARGET ${MY_PROJ_NAME}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/standalone/test.sh" "$<TARGET_FILE_DIR:${MY_PROJ_NAME}>/test.sh"
        COMMAND perl -pi -e "s/\\015//g" "$<TARGET_FILE_DIR:${MY_PROJ_NAME}>/test.sh"
        COMMAND chmod +x "$<TARGET_FILE_DIR:${MY_PROJ_NAME}>/test.sh"
        VERBATIM)
endif()
