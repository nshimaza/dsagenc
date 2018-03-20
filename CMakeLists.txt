cmake_minimum_required (VERSION 3.0.0)

project (dsagenc)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pthread")
set(DSLINK_DIR "${CMAKE_CURRENT_BINARY_DIR}/dslink")
include_directories("${DSLINK_DIR}/include")
include_directories("${DSLINK_DIR}/src/dslink/deps/libuv/include/")
include_directories("${DSLINK_DIR}/src/dslink-build/include/")
link_directories("${DSLINK_DIR}/src/dslink-build/")

add_executable(dsagenc main.c)
target_link_libraries(dsagenc sdk_dslink_c-static jansson libuv)


include(ExternalProject)

ExternalProject_Add(dslink
        PREFIX dslink
        GIT_REPOSITORY "https://github.com/IOT-DSA/sdk-dslink-c.git"
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}
        -DCMAKE_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/dslink
)
