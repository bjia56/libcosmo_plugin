cmake_minimum_required(VERSION 3.25)
project(test)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_subdirectory(${PROJECT_SOURCE_DIR}/libcosmo_plugin)

find_package (Threads REQUIRED)

if(BUILD_EXE)
    add_executable(cosmo.com
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
        ${LIBCOSMO_PLUGIN_SOURCES}
    )
    target_include_directories(cosmo.com PRIVATE
        ${LIBCOSMO_PLUGIN_INCLUDE_DIRS}
    )
    target_link_libraries(cosmo.com PUBLIC Threads::Threads)
else()
    add_library(native SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
        ${LIBCOSMO_PLUGIN_SOURCES}
    )
    target_include_directories(native PRIVATE
        ${LIBCOSMO_PLUGIN_INCLUDE_DIRS}
    )
    if(WIN32)
        target_link_libraries(native PUBLIC Threads::Threads wsock32 ws2_32)
    else()
        target_link_libraries(native PUBLIC Threads::Threads)
    endif()
endif()