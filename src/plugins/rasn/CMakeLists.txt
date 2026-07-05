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
if(TARGET dsn.dist.service.meta_server_lib)
    set(RASN_DIST_SERVICE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins_ext/rDSN.dist.service")
    list(APPEND MY_PROJ_INC_PATH
        "${RASN_DIST_SERVICE_DIR}/include"
        "${RASN_DIST_SERVICE_DIR}/src/common"
        "${RASN_DIST_SERVICE_DIR}/src/meta_server/meta_server_lib"
        "${RASN_DIST_SERVICE_DIR}/src/meta_server/zookeeper")
endif()

set(MY_PROJ_LIBS "")

set(MY_PROJ_LIB_PATH "")

set(MY_BINPLACES "")

dsn_add_static_library()

if(TARGET dsn.dist.service.meta_server_lib)
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
    target_link_libraries(rasn PUBLIC
        dsn.dist.service.meta_server_lib
        dsn.dist.service.replication_lib
        dsn.dist.service.common
        dsn.dist.service.fd.multimaster
        dsn.dist.service.fd
        dsn.dist.service.zookeeper_provider
        zookeeper_mt)

    # Ensure the dist.service providers (and their ExternalProject deps) are
    # built before rasn's consumers link.
    add_dependencies(rasn
        dsn.dist.service.meta_server_lib
        dsn.dist.service.replication_lib
        dsn.dist.service.common
        dsn.dist.service.fd.multimaster
        dsn.dist.service.fd
        dsn.dist.service.zookeeper_provider)
endif()

add_subdirectory(apps/codepilot)
add_subdirectory(apps/srepilot)
add_subdirectory(tests)
