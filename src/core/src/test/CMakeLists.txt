file(GLOB CORE_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../*.test.cpp")
file(GLOB CORE_PERF_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../*.perf.test.cpp")
file(GLOB CORE_TEST_RESOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/config-*.ini"
    "${CMAKE_CURRENT_SOURCE_DIR}/command.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/nfs_test_file*")
list(REMOVE_ITEM CORE_TEST_SOURCES ${CORE_PERF_TEST_SOURCES})

dsn_add_test_executable(
    NAME dsn.core.tests
    MAIN "${CMAKE_CURRENT_SOURCE_DIR}/core_test_main.cpp"
    SOURCES ${CORE_TEST_SOURCES}
    RESOURCES ${CORE_TEST_RESOURCES}
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/..")

dsn_register_test(
    NAME dsn.core.unit
    TARGET dsn.core.tests
    CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/test.config.core.ini"
    LABELS "unit;core"
    TIMEOUT 300
    RUN_SERIAL)

dsn_register_test(
    NAME dsn.core.fault
    TARGET dsn.core.tests
    CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/test.config.core.fj.ini"
    LABELS "unit;core;fault"
    TIMEOUT 300
    RUN_SERIAL)

dsn_add_test_executable(
    NAME dsn.core.perf_tests
    SOURCES ${CORE_PERF_TEST_SOURCES}
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/..")

dsn_register_test(
    NAME dsn.core.performance
    TARGET dsn.core.perf_tests
    CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/test.config.core.perf.ini"
    LABELS "performance;core"
    TIMEOUT 900
    RUN_SERIAL)
