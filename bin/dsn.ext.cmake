
#set(project_name googletest)
#set(target_url  https://github.com/linmajia/googletest.git)
#set(my_cmake_args "-Dgtest_force_shared_crt=OFF;")
#if(WIN32)
#    set(target_binaries gtest.lib gtest_main.lib)
#else()
#    set(target_binaries libgtest.a libgtest_main.a)
#endif()

include(ExternalProject)

string(TOUPPER ${project_name} PROJECT_NAME_U)

if (NOT DEFINED git_tag)
    set(get_tag "master")
endif()

set(target_bin_dir ${PROJECT_BINARY_DIR}/${project_name})
if (DEFINED target_bin_subdir)
    set(target_bin_subdir /${target_bin_subdir})
else ()
    set(target_bin_subdir "")
endif ()
set(target_install_dir ${PROJECT_BINARY_DIR}/lib)
set(target_copy_dir ${target_install_dir})
if(DEFINED target_install_subdir)
    set(target_copy_dir ${target_copy_dir}/${target_install_subdir})
endif()
set(install_cmd "")
if(NOT DEFINED exclude_from_all)
    set(exclude_from_all FALSE)
endif()

if(DEFINED skip_install AND skip_install)
    set(install_cmd ${CMAKE_COMMAND} -E echo "Skipping external project installation")
elseif(WIN32)
    if(EXISTS "${PROJECT_SOURCE_DIR}/bin/dsn.ext.copy.cmd")
        set(copy_cmd CALL ${PROJECT_SOURCE_DIR}/bin/dsn.ext.copy.cmd ${target_bin_dir}${target_bin_subdir} ${target_copy_dir} ${target_install_subdir})
    else()
        set(copy_cmd CALL $ENV{DSN_ROOT}/bin/dsn.ext.copy.cmd ${target_bin_dir}${target_bin_subdir} ${target_copy_dir} ${target_install_subdir})
    endif()
    set(install_cmd
        ${CMAKE_COMMAND} -E make_directory "${target_copy_dir}"
        COMMAND cmd /c ${copy_cmd})
else()
    set(install_cmd ${CMAKE_COMMAND} -E make_directory "${target_copy_dir}")
    foreach(file_i ${target_binaries})
        set(install_cmd
            ${install_cmd}
            COMMAND
                ${CMAKE_COMMAND} -E copy
                "${target_bin_dir}${target_bin_subdir}/${file_i}"
                "${target_copy_dir}")
    endforeach()
endif()

#message (INFO " install_cmd = ${install_cmd}")

ExternalProject_Add(${project_name}
    EXCLUDE_FROM_ALL ${exclude_from_all}
    GIT_REPOSITORY ${target_url}
    GIT_TAG ${git_tag}
    GIT_PROGRESS FALSE
    CMAKE_ARGS
        "${CMAKE_ARGS};-DCMAKE_INSTALL_PREFIX=${CMAKE_INSTALL_PREFIX};-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER};-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER};-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM};${my_cmake_args};"
    BINARY_DIR "${target_bin_dir}"
    INSTALL_DIR "${target_install_dir}"
    INSTALL_COMMAND ${install_cmd}
)

# Specify source dir
ExternalProject_Get_Property(${project_name} source_dir)
set(my_source_dir ${source_dir})

# Specify link libraries
ExternalProject_Get_Property(${project_name} binary_dir)
set(my_binary_dir ${binary_dir})
