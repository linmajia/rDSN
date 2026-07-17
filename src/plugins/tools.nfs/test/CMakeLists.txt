file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../*.test.cpp")

dsn_add_test_executable(
    NAME dsn.tools.nfs.tests
    SOURCES ${TEST_SOURCES}
    LIBRARIES dsn.tools.nfs
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/..")

dsn_register_test(
    NAME dsn.tools.nfs.unit
    TARGET dsn.tools.nfs.tests
    CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/test.config.tools.nfs.ini"
    LABELS "unit;plugin;tools.nfs"
    TIMEOUT 300
    RUN_SERIAL)
