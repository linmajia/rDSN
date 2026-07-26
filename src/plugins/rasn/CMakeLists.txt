if (DEFINED DSN_CMAKE_INCLUDED)
else()
    set(DSN_ROOT "$ENV{DSN_ROOT}")
    if(NOT EXISTS "${DSN_ROOT}/")
        message(FATAL_ERROR "Please make sure that ${DSN_ROOT} exists.")
    endif()

    include("${DSN_ROOT}/bin/dsn.cmake")
endif()

# The rASN platform is built as a static library that contains the reusable
# agent-system engine (runtime, services, workflow, observability, metrics,
# schema, ...). Both the CodePilot executable and the unit-test executable link
# this library instead of recompiling every engine source. Application entry
# points live with their executables (apps/*/main.cpp), so this directory
# intentionally contains no main().
set(MY_PROJ_NAME rasn)

# Engine source files under THIS directory are automatically included.
set(MY_PROJ_SRC "")

set(MY_SRC_SEARCH_MODE "GLOB")

# Allow engine sources and consumers to include rASN headers as
# `<rasn/<header>.h>`.
set(MY_PROJ_INC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/..")

# When the rDSN.dist.service plugin is part of the build, compile the rASN
# distributed coordination backend (coordination_service.cpp) against rDSN's
# distributed_lock_service / meta_state_service providers so ownership election
# and cluster-shared state are backed by real distributed facilities. Without
# that plugin, rASN still builds and runs with the in-process fallback only.
#
# The detection is deliberately CONFIGURE-ORDER INDEPENDENT: it must not use
# `if(TARGET dsn.dist.service.meta_server_lib)`, because rASN is configured from
# src/plugins/CMakeLists.txt, i.e. BEFORE src/plugins_ext. A target-existence
# probe would therefore always be false and would SILENTLY downgrade a
# --build_plugins build to the in-process fallback (it still compiles and links,
# so nothing would report the loss). Instead gate on the BUILD_PLUGINS cache
# variable -- which src/CMakeLists.txt uses to decide whether plugins_ext is
# configured at all, and which is visible in every directory scope -- plus the
# presence of the file that actually defines dsn.dist.service.meta_server_lib
# (that submodule may not be checked out).
set(RASN_DIST_SERVICE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins_ext/rDSN.dist.service")

set(RASN_HAS_DIST_SERVICE FALSE)
if(DEFINED BUILD_PLUGINS AND BUILD_PLUGINS STREQUAL "TRUE"
   AND EXISTS "${RASN_DIST_SERVICE_DIR}/src/meta_server/meta_server_lib/CMakeLists.txt")
    set(RASN_HAS_DIST_SERVICE TRUE)
endif()

if(RASN_HAS_DIST_SERVICE)
    list(APPEND MY_PROJ_INC_PATH
        "${RASN_DIST_SERVICE_DIR}/include"
        "${RASN_DIST_SERVICE_DIR}/src/common"
        "${RASN_DIST_SERVICE_DIR}/src/meta_server/meta_server_lib"
        "${RASN_DIST_SERVICE_DIR}/src/meta_server/zookeeper")
endif()

set(MY_PROJ_LIBS "")

set(MY_PROJ_LIB_PATH "")

set(MY_SERIALIZATION_TYPE "thrift")

set(MY_BINPLACES "")

dsn_add_static_library()

add_dependencies(rasn thrift)

if(RASN_HAS_DIST_SERVICE)
    # PUBLIC so the guard propagates to every consumer of the rasn library
    # (codepilot, srepilot, rasn.unit_tests). The coordination sources are
    # compiled here; a dependent that references the guard (the unit tests)
    # inherits it automatically instead of re-declaring and hand-syncing it.
    target_compile_definitions(rasn PUBLIC RASN_HAS_DIST_COORDINATION=1)

    # rasn is a static library, so coordination_service.cpp's references to the
    # rDSN distributed_lock_service / meta_state_service providers stay
    # unresolved until an executable links rasn. Expose the provider closure as a
    # PUBLIC link dependency of rasn so it propagates transitively to every
    # consumer in the correct dependents-before-dependencies order -- callers
    # only ever need to link `rasn`.
    #
    # These names refer to targets defined later in the configure run (under
    # src/plugins_ext). That is supported: CMake resolves plain, non-imported
    # target names given to target_link_libraries() at GENERATE time, and it
    # derives the build-order dependency from them as well. Do NOT add an
    # add_dependencies() call for the same targets -- that command requires the
    # targets to already exist and would hard-error from this directory.
    target_link_libraries(rasn PUBLIC
        dsn.dist.service.meta_server_lib
        dsn.dist.service.replication_lib
        dsn.dist.service.common
        dsn.dist.service.fd.multimaster
        dsn.dist.service.fd
        dsn.dist.service.zookeeper_provider
        zookeeper_mt)
endif()

add_subdirectory(apps/codepilot)
add_subdirectory(apps/srepilot)
add_subdirectory(tests)
