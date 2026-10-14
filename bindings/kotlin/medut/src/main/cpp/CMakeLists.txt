# JNI layer for the Kotlin binding, linked against the prebuilt core.
#
# libmedut.so comes prebuilt per ABI (scripts/build-android.sh); only this
# small shim is compiled here, so building the AAR needs no OpenSSL or
# baresip toolchain.
cmake_minimum_required(VERSION 3.22.1)
project(medut_jni C)

set(MEDUT_JNILIBS "" CACHE PATH "Directory holding <abi>/libmedut.so")
set(MEDUT_INCLUDE "" CACHE PATH "Directory holding medut.h")

add_library(medut SHARED IMPORTED)
set_target_properties(medut PROPERTIES
  IMPORTED_LOCATION "${MEDUT_JNILIBS}/${ANDROID_ABI}/libmedut.so"
  # The core is linked without an SONAME. Without this, CMake links it by
  # full path and that build-machine path ends up as libmedut_jni.so's
  # DT_NEEDED entry; with it the link is -lmedut and the entry is libmedut.so.
  IMPORTED_NO_SONAME TRUE)

add_library(medut_jni SHARED medut_jni.c)
target_include_directories(medut_jni PRIVATE "${MEDUT_INCLUDE}")
target_compile_options(medut_jni PRIVATE -Wall -Wextra -Werror)
# 16 KB pages (Android 15+ devices); the core is aligned the same way.
target_link_options(medut_jni PRIVATE "-Wl,-z,max-page-size=16384")
target_link_libraries(medut_jni PRIVATE medut)
