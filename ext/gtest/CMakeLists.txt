set(project_name googletest)

set(target_url ${GTEST_GIT_SOURCE})
set(git_tag ${GTEST_GIT_TAG})
set(target_bin_subdir src)
set(gtest_lib_dir "${PROJECT_BINARY_DIR}/${project_name}/src")
if(CMAKE_CONFIGURATION_TYPES)
    set(target_install_subdir "$<CONFIG>")
    if(NOT WIN32)
        set(target_bin_subdir "src/$<CONFIG>")
    endif()
endif()

set(my_cmake_args "-Wno-dev;-DCMAKE_SUPPRESS_DEVELOPER_WARNINGS=ON;-DCMAKE_POSITION_INDEPENDENT_CODE=ON;-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=${gtest_lib_dir};-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY=${gtest_lib_dir};-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${gtest_lib_dir};")

if(BUILD_TESTING)
    # Installed SDKs still support external plugins that register tests from
    # shared libraries. All test-enabled builds therefore provide one shared
    # GoogleTest registry and the dedicated legacy test host.
    set(my_cmake_args "${my_cmake_args};-DBUILD_SHARED_LIBS=ON;")
    set(skip_install FALSE)
    set(GTEST_LINKED_AS_SHARED_LIBRARY TRUE PARENT_SCOPE)
    if(WIN32)
        set(target_binaries gtest.dll gtest_main.dll)
    elseif(APPLE)
        set(target_binaries libgtest.dylib libgtest_main.dylib)
    else()
        set(target_binaries libgtest.so libgtest_main.so)
    endif()

else()
    set(my_cmake_args "${my_cmake_args};-DBUILD_SHARED_LIBS=OFF;")
    set(skip_install TRUE)
    if(WIN32)
        set(target_binaries gtest.lib gtest_main.lib)
    else()
        set(target_binaries libgtest.a libgtest_main.a)
    endif()
endif()

if(NOT BUILD_TESTING)
    set(exclude_from_all TRUE)
endif()

if(WIN32)
    set(my_cmake_args "${my_cmake_args};-Dgtest_force_shared_crt=OFF;-DCMAKE_POLICY_DEFAULT_CMP0091=NEW;-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>;")
endif()

include(${PROJECT_SOURCE_DIR}/bin/dsn.ext.cmake)

if(BUILD_TESTING)
    install(
        DIRECTORY ${my_source_dir}/include/gtest
        DESTINATION include/ext
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
        PATTERN "*.tcc"
        PATTERN "*.cpp")
endif()

set(GTEST_INCLUDE_DIR ${my_source_dir}/include PARENT_SCOPE)
set(GTEST_LIB_DIR ${PROJECT_BINARY_DIR}/lib PARENT_SCOPE)
