cmake_minimum_required(VERSION 3.22.1)
project(airplayfire C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(TP ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party ABSOLUTE)

# OpenSSL (libcrypto) from the Android NDK "thirdparty" prefab package.
find_package(openssl REQUIRED CONFIG)

# ---------------------------------------------------------------- libplist
file(GLOB PLIST_SRC
        ${TP}/libplist/src/*.c
        ${TP}/libplist/libcnary/*.c)
add_library(plist STATIC ${PLIST_SRC})
target_include_directories(plist
        PUBLIC ${TP}/libplist/include
        PRIVATE ${TP}/libplist/src ${TP}/libplist/libcnary/include)
target_compile_definitions(plist PRIVATE
        PACKAGE_VERSION="2.6.0"
        HAVE_STRNDUP=1
        HAVE_MEMMEM=1
        HAVE_TM_TM_GMTOFF=1
        HAVE_TM_TM_ZONE=1)
target_compile_options(plist PRIVATE -w)

# ---------------------------------------------------------------- UxPlay core
file(GLOB UXPLAY_SRC
        ${TP}/uxplay/lib/*.c
        ${TP}/uxplay/lib/llhttp/*.c
        ${TP}/uxplay/lib/playfair/*.c)
add_library(uxplay STATIC
        ${UXPLAY_SRC}
        ${CMAKE_CURRENT_SOURCE_DIR}/dnssd_shim/dns_sd_shim.c)
target_include_directories(uxplay
        PUBLIC ${TP}/uxplay/lib
        PRIVATE ${TP}/uxplay/lib/llhttp ${TP}/uxplay/lib/playfair
        ${CMAKE_CURRENT_SOURCE_DIR}/dnssd_shim)
target_compile_definitions(uxplay PRIVATE
        STANDALONE TARGET_POSIX _LINUX _REENTRANT NOHOLD
        __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS
        PLIST_210 PLIST_230
        OPENSSL_API_COMPAT=0x10101000L)
target_compile_options(uxplay PRIVATE -O2 -Wno-unused-variable -Wno-unused-function
        -Wno-deprecated-declarations -Wno-pointer-sign -Wno-unused-but-set-variable)
target_link_libraries(uxplay PUBLIC plist openssl::crypto)

# ---------------------------------------------------------------- JNI library
add_library(airplayfire SHARED
        jni_bridge.cpp
        video_decoder.cpp
        audio_player.cpp)
target_include_directories(airplayfire PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(airplayfire PRIVATE -O2 -Wall)
target_link_libraries(airplayfire
        uxplay
        mediandk
        aaudio
        android
        log)
