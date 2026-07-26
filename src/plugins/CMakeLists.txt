add_subdirectory(tools.common)
add_subdirectory(tools.nfs)
add_subdirectory(tools.emulator)
add_subdirectory(dist.uri.resolver)
add_subdirectory(apps.echo)
add_subdirectory(apps.skv)

if(BUILD_CSHARP)
    add_subdirectory(apps.echo.csharp)
endif()

# rASN (Robust Agent System Nucleus) builds unconditionally -- it is NOT gated by
# BUILD_PLUGINS. When src/plugins_ext is absent its CMake degrades to an
# in-process-only coordination facade, so the rASN apps still build and run
# locally. rASN's optional link against the plugins_ext dist.service providers is
# detected without a target-existence probe (see plugins/rasn/CMakeLists.txt), so
# it does not matter that plugins_ext is configured after this directory.
add_subdirectory(rasn)
