cmake_minimum_required(VERSION 3.15)
project(pdbconv LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(USE_BUNDLED_ZSTD "Use bundled zstd" OFF)

if(NOT USE_BUNDLED_ZSTD)
    find_package(zstd QUIET)
    if(NOT zstd_FOUND)
        message(WARNING "System zstd not found, falling back to bundled version")
        set(USE_BUNDLED_ZSTD ON)
    endif()
endif()

if(USE_BUNDLED_ZSTD)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "Build zstd programs")
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Build zstd tests")
    set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "Build zstd contrib")
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "Build zstd static library")
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "Build zstd shared library")
    add_subdirectory(extern/zstd/build/cmake EXCLUDE_FROM_ALL)
    # The bundled zstd defines libzstd_static
    if(NOT TARGET zstd::libzstd_static)
        add_library(zstd::libzstd_static ALIAS libzstd_static)
    endif()
endif()

add_subdirectory(ynwheaders)
add_subdirectory(pdbconv)
