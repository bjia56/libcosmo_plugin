cmake_minimum_required(VERSION 3.25)

project(cosmoRPC)

# Enable C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Add the source files
add_library(cosmoRPC
    src/cosmo_rpc.cpp
)

# Include header files
target_include_directories(cosmoRPC PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)