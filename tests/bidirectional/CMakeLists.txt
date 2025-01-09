cmake_minimum_required(VERSION 3.25)
project(test)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

add_subdirectory(${PROJECT_SOURCE_DIR}/libcosmo_plugin)

find_package (Threads REQUIRED)

if(BUILD_EXE)
    if(NOT DEFINED BINARY_NAME)
        set(BINARY_NAME "cosmo.com")
    endif()

    add_executable(${BINARY_NAME}
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
        ${LIBCOSMO_PLUGIN_SOURCES}
    )
else()
    if(NOT DEFINED BINARY_NAME)
        set(BINARY_NAME "native")
    endif()

    add_library(${BINARY_NAME} SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
        ${LIBCOSMO_PLUGIN_SOURCES}
    )
endif()

target_include_directories(${BINARY_NAME} PRIVATE
    ${LIBCOSMO_PLUGIN_INCLUDE_DIRS}
)
if(WIN32)
    target_link_libraries(${BINARY_NAME} PUBLIC Threads::Threads wsock32 ws2_32)
else()
    target_link_libraries(${BINARY_NAME} PUBLIC Threads::Threads)
endif()