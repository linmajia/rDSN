file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../*.test.cpp")

dsn_add_test_executable(
    NAME dsn.dist.uri.resolver.tests
    SOURCES ${TEST_SOURCES}
    LIBRARIES dsn.dist.uri.resolver
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/..")

dsn_register_test(
    NAME dsn.dist.uri.resolver.unit
    TARGET dsn.dist.uri.resolver.tests
    CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/test.config.dist.uri.resolver.ini"
    LABELS "unit;plugin;dist.uri.resolver"
    TIMEOUT 300
    RUN_SERIAL)
