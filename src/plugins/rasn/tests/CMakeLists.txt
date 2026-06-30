if (DEFINED DSN_CMAKE_INCLUDED)
else()
    set(DSN_ROOT "$ENV{DSN_ROOT}")
    if(NOT EXISTS "${DSN_ROOT}/")
        message(FATAL_ERROR "Please make sure that ${DSN_ROOT} exists.")
    endif()

    include("${DSN_ROOT}/bin/dsn.cmake")
endif()

set(RASN_UNIT_TEST_SUPPORT_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/../agent_clients.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../agent_registry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../agent_runtime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../agent_tools.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../agent_services.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../coordinator_service.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../codepilot/local_tools.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../llm_provider.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../observability.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../policy_manager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../rasn_core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../redaction.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../schema_manifest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../state_service.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../workflow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../workflow_service.cpp")

set(MY_PROJ_NAME rasn.unit_tests)

set(MY_PROJ_SRC
    ${RASN_UNIT_TEST_SUPPORT_SRC}
    "${CMAKE_CURRENT_SOURCE_DIR}/standalone/rasn_unit_test_main.cpp")

set(MY_SRC_SEARCH_MODE "GLOB")

set(MY_PROJ_INC_PATH
    ${GTEST_INCLUDE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/..")

set(MY_PROJ_LIBS gtest)

if (UNIX)
    set(MY_PROJ_LIBS ${MY_PROJ_LIBS} pthread)
endif()

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
