if(BUILD_TESTING)
    add_subdirectory(tests/support)
endif()

add_subdirectory(core)
add_subdirectory(dev)
add_subdirectory(tools)
add_subdirectory(plugins)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

if(BUILD_PLUGINS)
    add_subdirectory(plugins_ext)
endif()
