cmake_minimum_required(VERSION 3.25)
project(test)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_subdirectory(${PROJECT_SOURCE_DIR}/libcosmo_dlrpc)

if(BUILD_EXE)
    add_executable(cosmo.com
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
        ${COSMORPC_SOURCES}
    )

    target_include_directories(cosmo.com PRIVATE
        ${COSMORPC_INCLUDE_DIRS}
    )
else()
    add_library(native SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
        ${COSMORPC_SOURCES}
    )

    target_include_directories(native PRIVATE
        ${COSMORPC_INCLUDE_DIRS}
    )
endif()