# the minimum version of CMake.
cmake_minimum_required(VERSION 3.5.0)
project(openp2p)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

include_directories(${NATIVERENDER_ROOT_PATH}
                    ${NATIVERENDER_ROOT_PATH}/include)

# Do NOT link libopenp2p.so at build time.
# Linking makes libentry.so depend on it; HarmonyOS then fails to load libentry.so
# if dependency resolution order is wrong. Runtime dlopen() is used in napi_init.cpp.
#
# Place prebuilt Go library here (from OpenHarmony/build_so.sh):
#   entry/libs/arm64-v8a/libopenp2p.so
set(OPENP2P_LIB_DIR ${NATIVERENDER_ROOT_PATH}/../../../libs/arm64-v8a)
if(NOT EXISTS ${OPENP2P_LIB_DIR}/libopenp2p.so)
    message(WARNING "Missing ${OPENP2P_LIB_DIR}/libopenp2p.so — run bash OpenHarmony/build_so.sh first")
endif()

add_library(entry SHARED napi_init.cpp)
target_link_libraries(entry PUBLIC libace_napi.z.so libhilog_ndk.z.so)
target_link_options(entry PRIVATE -pthread)
