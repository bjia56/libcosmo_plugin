set(COSMORPC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reflect-cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reflect-cpp/include/rfl/thirdparty
    PARENT_SCOPE
)
set(COSMORPC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cosmo_dlrpc.cpp

    # https://rfl.getml.com/install/#option-4-include-source-files-into-your-own-build
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reflect-cpp/src/reflectcpp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reflect-cpp/src/reflectcpp_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reflect-cpp/src/yyjson.c

    PARENT_SCOPE
)