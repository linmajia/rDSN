add_library(dsn.test.support STATIC component_test_runner.cpp)
target_compile_definitions(
    dsn.test.support
    PRIVATE
        DSN_ENABLE_THRIFT_RPC
        DSN_USE_THRIFT_SERIALIZATION)
if(WIN32)
    target_compile_definitions(dsn.test.support PRIVATE DSN_TEST_IMPORTS)
endif()
if(GTEST_LINKED_AS_SHARED_LIBRARY)
    target_compile_definitions(
        dsn.test.support PRIVATE GTEST_LINKED_AS_SHARED_LIBRARY=1)
endif()
target_include_directories(
    dsn.test.support
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE
        ${GTEST_INCLUDE_DIR}
        ${THRIFT_INCLUDE_DIR})
add_dependencies(dsn.test.support googletest thrift)
set_target_properties(
    dsn.test.support
    PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/test/lib"
        POSITION_INDEPENDENT_CODE ON)
if(CMAKE_CONFIGURATION_TYPES)
    foreach(CONFIGURATION ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER "${CONFIGURATION}" CONFIGURATION_UPPER)
        set_target_properties(
            dsn.test.support
            PROPERTIES
                "ARCHIVE_OUTPUT_DIRECTORY_${CONFIGURATION_UPPER}" "${CMAKE_BINARY_DIR}/test/lib")
    endforeach()
endif()
