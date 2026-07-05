add_subdirectory(core)
add_subdirectory(dev)
add_subdirectory(tools)
add_subdirectory(tests)

# plugins_ext (rDSN.dist.service etc.) is configured before plugins so that
# in-tree plugins such as rasn can discover its targets (e.g. via if(TARGET ...))
# and declare build-order dependencies + linkage against them.
if(DEFINED BUILD_PLUGINS AND BUILD_PLUGINS STREQUAL "TRUE")
    add_subdirectory(plugins_ext)
endif()

add_subdirectory(plugins)
