add_subdirectory(tools.common)
add_subdirectory(tools.nfs)
add_subdirectory(tools.emulator)
add_subdirectory(dist.uri.resolver)
add_subdirectory(apps.echo)
add_subdirectory(apps.skv)
# rASN builds unconditionally here -- it does NOT require plugins_ext. Its CMake
# gracefully degrades: distributed coordination (the rDSN distributed_lock_service /
# meta_state_service backends) is only linked when `if(TARGET
# dsn.dist.service.meta_server_lib)` holds, i.e. after a `--build_plugins` build.
# Without plugins_ext the coordination facade compiles inproc-only, so the rASN
# apps still build and run locally. Do not gate this behind BUILD_PLUGINS.
add_subdirectory(rasn)

if(BUILD_CSHARP)
    add_subdirectory(apps.echo.csharp)
endif()
