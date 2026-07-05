add_subdirectory(core)
add_subdirectory(dev)
add_subdirectory(tools)
add_subdirectory(tests)
add_subdirectory(plugins)

if(DEFINED BUILD_PLUGINS AND BUILD_PLUGINS STREQUAL "TRUE")
    add_subdirectory(plugins_ext)
endif()

# rASN is configured LAST -- after both plugins and plugins_ext -- so that under a
# --build_plugins build the plugins_ext targets it optionally links against
# (dsn.dist.service.*) already exist and are discoverable via if(TARGET ...).
# plugins_ext does NOT depend on plugins, so this keeps rDSN's original
# plugins-then-plugins_ext order untouched. rASN builds unconditionally (it is not
# gated by BUILD_PLUGINS): without plugins_ext its CMake degrades to an inproc-only
# coordination facade, so the rASN apps still build and run locally.
add_subdirectory(plugins/rasn)
