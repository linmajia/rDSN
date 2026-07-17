if(WIN32)
    set(TEST_SOURCES "")
else()
    file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../*.test.cpp")
endif()

dsn_add_test_executable(
    NAME dsn.tools.emulator.tests
    SOURCES ${TEST_SOURCES}
    LIBRARIES dsn.tools.emulator
    INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tools.common")

dsn_register_test(
    NAME dsn.tools.emulator.unit
    TARGET dsn.tools.emulator.tests
    CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/test.config.tools.emulator.ini"
    LABELS "unit;plugin;tools.emulator"
    TIMEOUT 300
    RUN_SERIAL)
