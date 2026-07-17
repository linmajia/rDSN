file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../*.test.cpp")

dsn_add_test_executable(
    NAME dsn.tools.common.tests
    SOURCES ${TEST_SOURCES}
    LIBRARIES dsn.tools.common
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/..")

dsn_register_test(
    NAME dsn.tools.common.unit
    TARGET dsn.tools.common.tests
    CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/test.config.tools.common.ini"
    LABELS "unit;plugin;tools.common"
    TIMEOUT 300
    RUN_SERIAL)
