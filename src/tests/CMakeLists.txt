# External plugin repositories still embed their test registrations. Install a
# test-only host with every test-enabled SDK so standalone plugin builds can run
# those configurations against the shared GoogleTest registry.
dsn_add_test_executable(NAME dsn.legacy.tests)
install(TARGETS dsn.legacy.tests RUNTIME DESTINATION bin/dsn.legacy.tests)
if(CMAKE_CONFIGURATION_TYPES)
    install(
        TARGETS dsn.legacy.tests
        RUNTIME DESTINATION "bin/dsn.legacy.tests/$<CONFIG>")
endif()

add_subdirectory(idl)
