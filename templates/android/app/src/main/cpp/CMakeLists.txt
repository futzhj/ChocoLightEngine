cmake_minimum_required(VERSION 3.18)
project(ChocoLightJNI LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)

# Lumen 源码目录
# 从 templates/android/app/src/main/cpp/ 到仓库根: ../../../../../../
set(LUMEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../lumen-master")

# 直接编译 Lumen 核心源码为静态库（避免 .so 依赖问题）
file(GLOB LUMEN_CORE_SRC "${LUMEN_DIR}/lib/lumen/*.cpp")
file(GLOB LUMEN_LUA_SRC  "${LUMEN_DIR}/lib/lua/*.cpp")
# 排除 load_platform.c (C file)
file(GLOB LUMEN_LUA_C_SRC "${LUMEN_DIR}/lib/lua/*.c")

add_library(lumen-embedded STATIC
    ${LUMEN_CORE_SRC}
    ${LUMEN_LUA_SRC}
    ${LUMEN_LUA_C_SRC}
)

target_include_directories(lumen-embedded PUBLIC
    "${LUMEN_DIR}/include"
    "${LUMEN_DIR}/internal"
)

target_compile_definitions(lumen-embedded PRIVATE
    LUA_USE_LINUX  # POSIX APIs for Android
)

# JNI 共享库（最终打入 APK）
add_library(chocolight SHARED
    light_jni.cpp
)

target_include_directories(chocolight PRIVATE
    "${LUMEN_DIR}/include"
)

target_link_libraries(chocolight PRIVATE
    lumen-embedded
    android
    log
)
