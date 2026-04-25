cmake_minimum_required(VERSION 3.20)
project(ChocoLightAndroid LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==================== 路径配置 ====================
# 从 templates/android-sdl3/app/src/main/cpp/ 到仓库根: ../../../../../..
# templates(1)/android-sdl3(2)/app(3)/src(4)/main(5)/cpp(6) → 6 层
set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..")
set(LUMEN_DIR "${ROOT_DIR}/lumen-master")
set(CHOCOLIGHT_DIR "${ROOT_DIR}/ChocoLight")

# ==================== Lumen (嵌入式静态库) ====================
# 直接编译 Lumen 核心源码, 避免 .so 依赖问题
file(GLOB LUMEN_CORE_SRC "${LUMEN_DIR}/lib/lumen/*.cpp")
file(GLOB LUMEN_LUA_SRC  "${LUMEN_DIR}/lib/lua/*.cpp")
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
    LUA_USE_LINUX   # POSIX APIs for Android
    LUA_USE_COMPAT  # Lua 5.1 兼容模式
)

# ==================== ChocoLight 引擎 ====================
# 作为子项目编译 (会 FetchContent SDL3 + 编译引擎为 STATIC lib)
set(LUMEN_LIB_PATH lumen-embedded CACHE STRING "" FORCE)
add_subdirectory(${CHOCOLIGHT_DIR} ${CMAKE_BINARY_DIR}/ChocoLight)

# ==================== SDL3 Java 源码路径导出 ====================
# FetchContent 下载的 SDL3 源码中包含 Java 文件
# Gradle 需要这个路径来编译 SDLActivity.java
set(SDL3_JAVA_DIR "${FETCHCONTENT_BASE_DIR}/sdl3-src/android-project/app/src/main/java")
message(STATUS "SDL3 Java sources: ${SDL3_JAVA_DIR}")

# ==================== 主入口共享库 ====================
# APK 加载的最终 .so: 包含 main() + 链接 Light + Lumen + SDL3
add_library(chocolight SHARED
    main.cpp
)

target_include_directories(chocolight PRIVATE
    "${LUMEN_DIR}/include"
    "${CHOCOLIGHT_DIR}/include"
    "${ROOT_DIR}/templates/shared"
)

target_link_libraries(chocolight PRIVATE
    Light
    lumen-embedded
    SDL3::SDL3
    GLESv3 EGL android log
)
