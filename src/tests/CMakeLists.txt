if(BUILD_PLUGINS)
    # External plugin repositories still embed their test registrations. Run
    # those configurations in a test-only host that owns the shared GTest runner.
    dsn_add_test_executable(NAME dsn.legacy.tests)
endif()

add_subdirectory(idl)
