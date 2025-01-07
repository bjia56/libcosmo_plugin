cmake_minimum_required(VERSION 3.25)

project(cosmoRPC)

# Enable C++14 standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED True)

# Add the source files
add_library(cosmoRPC
    src/cosmo_rpc.cpp
)

# Include header files
target_include_directories(cosmoRPC PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

# Add third party libraries
add_subdirectory(
    ${PROJECT_SOURCE_DIR}/third_party/json
)

# Link the libraries
target_link_libraries(cosmoRPC PRIVATE nlohmann_json::nlohmann_json)