INCLUDE(TestBigEndian)
TEST_BIG_ENDIAN(BIGENDIAN)
IF (BIGENDIAN)
    SET(MSGPACK_ENDIAN_BIG_BYTE 1)
    SET(MSGPACK_ENDIAN_LITTLE_BYTE 0)
ELSE ()
    SET(MSGPACK_ENDIAN_BIG_BYTE 0)
    SET(MSGPACK_ENDIAN_LITTLE_BYTE 1)
ENDIF ()

CONFIGURE_FILE (
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msgpack-c/cmake/sysdep.h.in
    include/msgpack/sysdep.h
    @ONLY
)

CONFIGURE_FILE (
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msgpack-c/cmake/pack_template.h.in
    include/msgpack/pack_template.h
    @ONLY
)

set(LIBCOSMO_PLUGIN_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reflect-cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reflect-cpp/include/rfl/thirdparty
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msgpack-c/include

    # for generated sysdep.h
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/include/msgpack
    PARENT_SCOPE
)
set(LIBCOSMO_PLUGIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cosmo_plugin.cpp

    # https://rfl.getml.com/install/#option-4-include-source-files-into-your-own-build
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reflect-cpp/src/reflectcpp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reflect-cpp/src/reflectcpp_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reflect-cpp/src/reflectcpp_msgpack.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reflect-cpp/src/yyjson.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msgpack-c/src/objectc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msgpack-c/src/unpack.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msgpack-c/src/version.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msgpack-c/src/vrefbuffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/msgpack-c/src/zone.c
    PARENT_SCOPE
)