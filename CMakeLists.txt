#  HMat-OSS (HMatrix library, open source software)
#
#  Copyright (C) 2014-2015 Airbus Group SAS
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#
#  http://github.com/jeromerobert/hmat-oss

cmake_minimum_required(VERSION 3.3)

# Set CMAKE_BUILD_TYPE to Release by default.
# Must be done before calling project()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: None(CMAKE_CXX_FLAGS or CMAKE_C_FLAGS used) Debug Release RelWithDebInfo MinSizeRel." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "" Release Debug RelWithDebInfo MinSizeRel)
endif()

# Set BUILD_SHARED_LIBS to ON by default.
# Must be done before calling project()
if(BUILD_SHARED_LIBS MATCHES "^BUILD_SHARED_LIBS$")
    if(NOT WIN32)
        # __declspec(dllexport) are missing in hmat so it's currently not possible
        # to build it as a shared library on win32
        set(BUILD_SHARED_LIBS "ON" CACHE BOOL "Build shared libraries." FORCE)
    endif()
endif()

project(hmat-oss C CXX)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${PROJECT_SOURCE_DIR}/CMake)

if(NOT HMAT_VERSION)
    include(GitVersion)
    git_version(HMAT 1.5.0)
endif()
set(HMAT_SO_VERSION 1)

# Offer the user the choice of overriding the installation directories
set(INSTALL_LIB_DIR     lib${LIB_SUFFIX} CACHE PATH "Installation directory for libraries")
set(INSTALL_BIN_DIR     bin              CACHE PATH "Installation directory for executables")
set(INSTALL_INCLUDE_DIR include          CACHE PATH "Installation directory for header files")
set(INSTALL_DATA_DIR    share/hmat       CACHE PATH "Installation directory for data files")
set(INSTALL_CMAKE_DIR   lib${LIB_SUFFIX}/cmake/hmat CACHE PATH "Installation directory for cmake config files")

# Make relative paths absolute (needed later on)
foreach(p LIB BIN INCLUDE DATA CMAKE)
    set(var INSTALL_${p}_DIR)
    set(RELATIVE_INSTALL_${p}_DIR ${INSTALL_${p}_DIR})
    if(NOT IS_ABSOLUTE "${${var}}")
        set(${var} "${CMAKE_INSTALL_PREFIX}/${${var}}")
    endif()
endforeach()

# ========================
# Declare targets
# ========================

# Options which modify the list of source files
option(HMAT_TIMELINE "Enable profiling timeline" OFF)

# Sources
file(GLOB_RECURSE HMAT_SOURCES RELATIVE ${PROJECT_SOURCE_DIR} src/*.[ch]pp include/*.h)
list(REMOVE_ITEM HMAT_SOURCES src/hmat_cpp_interface.cpp src/recursion.cpp) # because it is included in default_engine.cpp (and the other *_engine.cpp) and h_matrix.cpp
if(NOT HMAT_TIMELINE)
    list(REMOVE_ITEM HMAT_SOURCES src/common/timeline.cpp)
endif()

# lib HMAT
if(HMAT_LIBRARIES)
    add_library(${PROJECT_NAME} OBJECT ${HMAT_SOURCES})
    set_target_properties(${PROJECT_NAME} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(${PROJECT_NAME}_VISIBILITY "PUBLIC")
else()
    add_library(${PROJECT_NAME} ${HMAT_SOURCES})
    set(${PROJECT_NAME}_VISIBILITY "PRIVATE")
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES DEFINE_SYMBOL HMAT_DLL_EXPORTS)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
    PUBLIC
        $<INSTALL_INTERFACE:${RELATIVE_INSTALL_INCLUDE_DIR}>
)

# ========================
# C
# ========================
include(CheckIncludeFile)
check_include_file("stdint.h" HAVE_STDINT_H)
check_include_file("sys/types.h" HAVE_SYS_TYPES_H)
check_include_file("time.h" HAVE_TIME_H)
check_include_file("sys/resource.h" HAVE_SYS_RESOURCE_H)
check_include_file("unistd.h" HAVE_UNISTD_H)
check_include_file("mach/mach_time.h" HAVE_MACH_MACH_TIME_H)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
   set(HMAT_32BITS TRUE)
endif()

# ========================
# System & external libs
# ========================
include(CheckLibraryExists)

check_library_exists("m" sqrt "" HAVE_LIBM)
if(HAVE_LIBM)
    target_link_libraries(${PROJECT_NAME} ${${PROJECT_NAME}_VISIBILITY} m)
    # Examples will also have to be linked against libm
    set(LIBM_TARGET m)
endif()

check_library_exists("rt" clock_gettime "" HAVE_LIBRT)
if(HAVE_LIBRT)
    # clock_gettime() function is called from src/common/chrono.h
    # and must thus be PUBLIC.
    # FIXME: link is mandatory only for GNU libc < 2.17.
    #        Maybe we can drop this line?
    target_link_libraries(${PROJECT_NAME} PUBLIC rt)
endif()

option(HMAT_DISABLE_OPENMP "Let HMat disable OpenMP (require OpenMP support)" ON)
if(HMAT_DISABLE_OPENMP)
    find_package(OpenMP)
    if(OpenMP_FOUND)
        # OpenMP requires that consumers also link against it, this is why
        # it is public; and find_dependency(OpenMP) is added into HMatConfig.cmake
        # to ensure that consumers also define OpenMP::OpenMP_C target.
        target_compile_options(${PROJECT_NAME} PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:${OpenMP_CXX_FLAGS}>$<$<COMPILE_LANGUAGE:C>:${OpenMP_C_FLAGS}>")
        target_link_libraries(${PROJECT_NAME} PUBLIC "${OpenMP_C_FLAGS}")
    endif()
endif()

# JeMalloc
option(HMAT_JEMALLOC "Use jemalloc for matrix blocks allocation." OFF)
if(HMAT_JEMALLOC)
    find_package(JeMalloc REQUIRED)
    if(JEMALLOC_FOUND)
        set(HAVE_JEMALLOC TRUE)
        target_include_directories(${PROJECT_NAME} PRIVATE "${JEMALLOC_INCLUDE_DIR}")
        target_link_libraries(${PROJECT_NAME} ${${PROJECT_NAME}_VISIBILITY} "${JEMALLOC_LIBRARIES}")
    endif()
endif()

# Context timers
option(HMAT_CONTEXT "Use context timers." OFF)
if(HMAT_CONTEXT)
    message(STATUS "Use context timers")
    set(HAVE_CONTEXT TRUE)
endif()

# ========================
# BLAS/MKL
# ========================

find_package(MKL)
if(MKL_FOUND)
  # FIXME: Replace those set() commands by target_link_libraries; needs rework of FindMKL.cmake
  set(CMAKE_C_FLAGS "${MKL_COMPILER_FLAGS} ${CMAKE_C_FLAGS}")
  set(CMAKE_CXX_FLAGS "${MKL_COMPILER_FLAGS} ${CMAKE_CXX_FLAGS}")
  set(CBLAS_INCLUDE_DIR ${MKL_INCLUDE_DIRS})
  # The start-group/end-group flags must be at the end of the
  # link command line, so we must use target_link_libraries not, CMAKE_SHARED_XXX_FLAGS
  target_link_libraries(${PROJECT_NAME} ${${PROJECT_NAME}_VISIBILITY} ${MKL_LINKER_FLAGS})
endif()

option(USE_DEBIAN_OPENBLAS "On Debian, link to openblas instead of generic blas." ON)
# BLAS/LAPACK
if (NOT MKL_FOUND)
   if(USE_DEBIAN_OPENBLAS)
        get_filename_component(real_blas_path "/usr/lib/libblas.so" REALPATH)
        string(REGEX MATCH "/usr/lib/openblas-base/libblas.so" is_debian_openblas ${real_blas_path})
        if(is_debian_openblas)
            set(BLAS_FOUND ON)
            set(BLAS_LIBRARIES openblas)
            set(CBLAS_INCLUDE_DIR /usr/include/openblas)
            set(LAPACK_LIBRARIES openblas)
            unset(BLAS_LINKER_FLAGS)
        endif()
    endif()
    if(NOT is_debian_openblas)
        find_package(BLAS REQUIRED)
        find_package(LAPACK REQUIRED)
    endif()
endif()

# CBLAS

# For backward compatibility
if(CBLAS_INCLUDE_DIRS)
    set(CBLAS_INCLUDE_DIR ${CBLAS_INCLUDE_DIRS})
endif()

if (NOT MKL_CBLAS_FOUND)
    include(CMakePushCheckState)
    cmake_push_check_state()
    set(CMAKE_REQUIRED_LIBRARIES ${BLAS_LIBRARIES})
    include(CheckFunctionExists)
    check_function_exists("openblas_set_num_threads" HAVE_OPENBLAS_SET_NUM_THREADS)
    check_function_exists("goto_get_num_procs" HAVE_GOTO_GET_NUM_PROCS)
    check_function_exists("cblas_dgemm" CHECK_FUNCTION_EXISTS_CBLAS_DGEMM)
    cmake_pop_check_state()
endif()
if ((NOT MKL_CBLAS_FOUND) AND (NOT is_debian_openblas))
    # Functions may already be available via MKL or BLAS, but we need cblas.h
    if (CHECK_FUNCTION_EXISTS_CBLAS_DGEMM)
        find_path(CBLAS_INCLUDE_DIR NAMES cblas.h DOC "CBLAS include directory")
        if(CBLAS_INCLUDE_DIR)
            set(CMAKE_REQUIRED_INCLUDES ${CBLAS_INCLUDE_DIR})
            check_include_file("cblas.h" HAVE_CBLAS_H)
            if (NOT HAVE_CBLAS_H)
                message(FATAL_ERROR "cblas.h not found")
            endif()
        else(CBLAS_INCLUDE_DIR)
            message(FATAL_ERROR "cblas.h not found")
        endif(CBLAS_INCLUDE_DIR)
    else (CHECK_FUNCTION_EXISTS_CBLAS_DGEMM)
        find_package(CBLAS REQUIRED)
    endif()
    target_include_directories(${PROJECT_NAME} PRIVATE ${CBLAS_INCLUDE_DIR})
endif()

if(CMAKE_VERSION VERSION_GREATER 3.12)
    if(BUILD_SHARED_LIBS)
        target_link_options(${PROJECT_NAME} ${${PROJECT_NAME}_VISIBILITY} "${LAPACK_LINKER_FLAGS};${BLAS_LINKER_FLAGS}")
    else()
        set_target_properties(${PROJECT_NAME} PROPERTIES STATIC_LIBRARY_OPTIONS "${LAPACK_LINKER_FLAGS};${BLAS_LINKER_FLAGS}")
    endif()
else()
    set_target_properties(${PROJECT_NAME} PROPERTIES LINK_FLAGS "${LAPACK_LINKER_FLAGS} ${BLAS_LINKER_FLAGS}")
endif()

target_link_libraries(${PROJECT_NAME} ${${PROJECT_NAME}_VISIBILITY}
        ${LAPACK_LIBRARIES} ${CBLAS_LIBRARIES} ${BLAS_LIBRARIES})

# ========================
# Compiler flags
# ========================
check_cxx_compiler_flag("-Werror -Wall -Wno-unused-result -Wno-sign-compare -Wno-literal-suffix" HAVE_GCC_WARNING_FLAGS)
if(HAVE_GCC_WARNING_FLAGS)
  target_compile_options(${PROJECT_NAME} PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-Werror -Wall -Wno-unused-result -Wno-sign-compare -Wno-literal-suffix>
      $<$<COMPILE_LANGUAGE:C>:-Werror -Wall -Wno-sign-compare>)
endif()
check_cxx_compiler_flag("-Wno-undefined-var-template" DISABLE_CLANG_FALSE_POSITIVES)
if(DISABLE_CLANG_FALSE_POSITIVES)
  target_compile_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-undefined-var-template>)
endif()

# Enable gcc vectorization
function(opt_flag flag)
  include(CheckCCompilerFlag)
  string(MAKE_C_IDENTIFIER ${flag} label)
  check_c_compiler_flag(${flag} ${label})
  if(${label})
     target_compile_options(${PROJECT_NAME} PRIVATE $<$<NOT:$<CONFIG:Debug>>:${flag}>)
  endif()
endfunction()
opt_flag("-ffast-math")
opt_flag("-funsafe-math-optimizations")

# Specific flags for MSVC
target_compile_definitions(${PROJECT_NAME} PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:__func__=__FUNCTION__;_CRT_SECURE_NO_WARNINGS;NOMINMAX>")

# FIXME: why do we need c_std_99 on Windows Intel?
target_compile_features(${PROJECT_NAME} PRIVATE $<$<AND:$<C_COMPILER_ID:Intel>,$<PLATFORM_ID:Windows>>:c_std_99>)

# ========================
# Misc
# ========================

# Build date export through hmat_get_build_date()
option(HMAT_EXPORT_BUILD_DATE "Hmat exports build date." OFF)
if(HMAT_EXPORT_BUILD_DATE)
    message(STATUS "Exports build date")
endif()

if(HMAT_EXPORT_BUILD_DATE AND NOT WIN32)
  # Regle pour toujours recompiler les sources contenant __DATE__ et __TIME__
  add_custom_command(COMMAND ${CMAKE_COMMAND} -E touch ${PROJECT_BINARY_DIR}/forcedBuild.tmp
    OUTPUT ${PROJECT_BINARY_DIR}/forcedBuild.tmp)
  set_source_files_properties( src/c_default_interface.cpp PROPERTIES OBJECT_DEPENDS ${PROJECT_BINARY_DIR}/forcedBuild.tmp)
endif(HMAT_EXPORT_BUILD_DATE AND NOT WIN32)

if(NOT HMAT_NO_VERSION)
  set_target_properties(${PROJECT_NAME} PROPERTIES VERSION ${HMAT_VERSION} SOVERSION ${HMAT_SO_VERSION})
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES INSTALL_NAME_DIR ${INSTALL_LIB_DIR})

# ========================
# Configuration file
# ========================
if(NOT BUILD_SHARED_LIBS)
    set(HMAT_STATIC ON)
endif()
configure_file("${PROJECT_SOURCE_DIR}/CMake/config.h.in" "${CMAKE_CURRENT_BINARY_DIR}/config.h")
configure_file("${PROJECT_SOURCE_DIR}/CMake/hmat-config-oss.h.in" "${CMAKE_CURRENT_BINARY_DIR}/hmat/config-oss.h")
if(NOT HMAT_SKIP_CONFIG)
  configure_file("${PROJECT_SOURCE_DIR}/CMake/hmat-config.h.in" "${CMAKE_CURRENT_BINARY_DIR}/hmat/config.h")
endif()

# ========================
# Examples
# ========================

option(BUILD_EXAMPLES "build examples" OFF)
option(INSTALL_EXAMPLES "install examples" OFF)

# Install examples with RPATH
list(FIND CMAKE_PLATFORM_IMPLICIT_LINK_DIRECTORIES "${INSTALL_LIB_DIR}" isSystemDir)
if("${isSystemDir}" STREQUAL "-1")
    set(CMAKE_INSTALL_RPATH "${INSTALL_LIB_DIR}")
endif("${isSystemDir}" STREQUAL "-1")

macro(hmat_add_example name source)
  if (BUILD_EXAMPLES)
    if (MSVC)
      set_source_files_properties("examples/${source}" PROPERTIES COMPILE_DEFINITIONS "_USE_MATH_DEFINES")
      #  No complex.h on MSVC, compile with C++
      set_source_files_properties("examples/${source}" PROPERTIES LANGUAGE CXX)
    endif ()
    add_executable(${HMAT_PREFIX_EXAMPLE}${name} ${PROJECT_SOURCE_DIR}/examples/${source})
    target_include_directories(${HMAT_PREFIX_EXAMPLE}${name}
        PRIVATE
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
            $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}>
    )
    target_link_libraries(${HMAT_PREFIX_EXAMPLE}${name} PRIVATE ${PROJECT_NAME} ${LIBM_TARGET})
    if (INSTALL_EXAMPLES)
        install(TARGETS ${HMAT_PREFIX_EXAMPLE}${name}
                DESTINATION "${RELATIVE_INSTALL_BIN_DIR}/examples"
                COMPONENT Runtime
        )
    endif ()
  endif ()
endmacro()

hmat_add_example(kriging kriging.cpp)
hmat_add_example(c-cylinder c-cylinder.c)
hmat_add_example(c-simple-cylinder c-simple-cylinder.c)
hmat_add_example(c-simple-kriging c-simple-kriging.c)
hmat_add_example(c-cholesky c-cholesky.c)

if (BUILD_EXAMPLES)
  enable_testing ()
  add_test (NAME cholesky COMMAND ${HMAT_PREFIX_EXAMPLE}c-cholesky 1000 S)
  add_test (NAME cylinder COMMAND ${HMAT_PREFIX_EXAMPLE}c-cylinder 1000 Z)
  add_test (NAME simple-cylinder COMMAND ${HMAT_PREFIX_EXAMPLE}c-simple-cylinder 1000 Z)
endif ()

# ========================
# Install
# ========================

install(DIRECTORY include/hmat DESTINATION "${RELATIVE_INSTALL_INCLUDE_DIR}" COMPONENT Development)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hmat/config-oss.h DESTINATION "${RELATIVE_INSTALL_INCLUDE_DIR}/hmat" COMPONENT Development)
if(NOT HMAT_SKIP_CONFIG)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hmat/config.h DESTINATION "${RELATIVE_INSTALL_INCLUDE_DIR}/hmat" COMPONENT Development)
endif()
set(CXX_HMAT_HEADERS "full_matrix;hmat_cpp_interface;compression;h_matrix;"
    "coordinates;clustering;admissibility;default_engine;cluster_tree;tree;assembly;data_types")
foreach(header ${CXX_HMAT_HEADERS})
    set(HMAT_HEADERS src/${header}.hpp;${HMAT_HEADERS})
endforeach()
install(FILES ${HMAT_HEADERS} DESTINATION "${RELATIVE_INSTALL_INCLUDE_DIR}" COMPONENT Development)

# ========================
# Export
# ========================

if(NOT DEFINED HMAT_LIBRARIES)
    set(HMAT_LIBRARIES ${PROJECT_NAME})

    # We want users to link agains HMAT::hmat target
    set_target_properties(${PROJECT_NAME} PROPERTIES EXPORT_NAME hmat)

    # Define our exported target
    install(TARGETS ${PROJECT_NAME}
            EXPORT HMAT-targets
            RUNTIME
                DESTINATION "${RELATIVE_INSTALL_BIN_DIR}"
                COMPONENT Runtime
            LIBRARY
                DESTINATION "${RELATIVE_INSTALL_LIB_DIR}"
                COMPONENT Runtime
            ARCHIVE
                DESTINATION "${RELATIVE_INSTALL_LIB_DIR}"
                COMPONENT Development
            INCLUDES
                DESTINATION "${RELATIVE_INSTALL_INCLUDE_DIR}"
    )

    # Allow using HMATTargets.cmake from the build tree
    export(EXPORT HMAT-targets NAMESPACE "HMAT::" FILE HMATTargets.cmake)

    # Create HMATConfig.cmake and HMATConfigVersion.cmake files

    # Define these variables for users which have not yet migrated;
    # when support for these variables are deprecated, we can copy
    # call configure_file with COPYONLY option
    set(HMAT_INCLUDE_DIRS "${INSTALL_INCLUDE_DIR}")
    set(HMAT_LIBRARY_DIRS ${INSTALL_LIB_DIR})
    set(HMAT_HAVE_OPENMP ${OpenMP_FOUND})
    configure_file(CMake/HMATConfig.cmake.in "${PROJECT_BINARY_DIR}/HMATConfig.cmake" @ONLY)

    include(CMakePackageConfigHelpers)
    write_basic_package_version_file("${PROJECT_BINARY_DIR}/HMATConfigVersion.cmake"
        VERSION ${HMAT_VERSION} COMPATIBILITY AnyNewerVersion)
    install(FILES
          ${PROJECT_BINARY_DIR}/HMATConfig.cmake
          ${PROJECT_BINARY_DIR}/HMATConfigVersion.cmake
        DESTINATION ${RELATIVE_INSTALL_CMAKE_DIR})

    install(EXPORT HMAT-targets NAMESPACE "HMAT::" FILE HMATTargets.cmake
            DESTINATION ${RELATIVE_INSTALL_CMAKE_DIR} COMPONENT Development)

else()
    # It's a mess to detect so we publish it up (temporary solution)
    set(HMAT_CBLAS_INCLUDE_DIR ${CBLAS_INCLUDE_DIR} PARENT_SCOPE)
    set(HMAT_CBLAS_LIBRARIES "${BLAS_LIBRARIES};${CBLAS_LIBRARIES}" PARENT_SCOPE)
endif()

# ========================
# final LOG
# ========================
include(FeatureSummary)
feature_summary(WHAT ALL)
