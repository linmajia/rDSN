if (DEFINED DSN_CMAKE_INCLUDED)
else()
    
    set(DSN_ROOT "$ENV{DSN_ROOT}")
    if(NOT EXISTS "${DSN_ROOT}/")
        message(FATAL_ERROR "Please make sure that ${DSN_ROOT} exists.")
    endif()

    include("${DSN_ROOT}/bin/dsn.cmake")
endif()

set(MY_PROJ_NAME dsn.tools.emulator)

# Source files under CURRENT project directory will be automatically included.
# Compile the native AIO provider from tools.common to avoid maintaining a copy here.
if(WIN32)
    set(MY_PROJ_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../tools.common/native_aio_provider.win.cpp")
else()
    set(MY_PROJ_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../tools.common/native_aio_provider.posix.cpp")
endif()

# Search mode for source files under CURRENT project directory?
# "GLOB_RECURSE" for recursive search
# "GLOB" for non-recursive search
set(MY_SRC_SEARCH_MODE "GLOB")
if(WIN32 AND BUILD_TESTING)
    # Keep scheduler tests in the DLL that owns its inline singleton and
    # extension-slot state. The shared GoogleTest registry exposes them to the
    # standalone host after this module is loaded from the test configuration.
    set(MY_PROJ_EXCLUDE_REGEXES "")
    set(MY_PROJ_LIBS gtest)
else()
    set(MY_PROJ_EXCLUDE_REGEXES "\\.test\\.cpp$")
    set(MY_PROJ_LIBS "")
endif()

set(MY_PROJ_INC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../tools.common")

set(MY_BOOST_PACKAGES "")

set(MY_PROJ_LIB_PATH "")

# Extra files that will be installed
set(MY_BINPLACES "")

dsn_add_shared_library()

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
