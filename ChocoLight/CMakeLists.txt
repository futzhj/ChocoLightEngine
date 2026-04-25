cmake_minimum_required(VERSION 3.20)
project(ChocoLight LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==================== SDL3 (FetchContent) ====================
# 从官方仓库拉取源码编译, 锁定稳定版 release-3.2.0
# 静态链接到 Light.dll/so/dylib, 避免运行时动态库依赖

include(FetchContent)

# SDL3 编译选项: 只要 video+events+gl, 不要 audio (我们用 miniaudio)/test/examples
set(SDL_SHARED        OFF CACHE BOOL "" FORCE)
set(SDL_STATIC        ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)
set(SDL_TESTS         OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO         OFF CACHE BOOL "" FORCE)
set(SDL_RENDER        OFF CACHE BOOL "" FORCE)  # 我们不用 SDL_Renderer (用 GL)
set(SDL_GPU           OFF CACHE BOOL "" FORCE)  # M1 不用 SDL_GPU

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)

message(STATUS "SDL3: integrated via FetchContent (release-3.2.0)")

# ==================== 路径配置 ====================

# Lua 5.1 SDK 路径
set(LUA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../lumen-master/include")

# SQLite3 amalgamation
set(SQLITE3_DIR "${CMAKE_SOURCE_DIR}/third_party/sqlite3")

# glad GL 3.3 Core Profile 加载器
set(GLAD_DIR "${CMAKE_SOURCE_DIR}/third_party/glad")

# libuv 异步 IO 库 (网络抽象)
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libuv EXCLUDE_FROM_ALL)

# ==================== Lumen (lua51) 库路径 ====================

# 允许外部指定, 否则使用默认路径
if(NOT DEFINED LUMEN_LIB_PATH)
    if(WIN32)
        set(LUMEN_LIB_PATH "${CMAKE_SOURCE_DIR}/../lumen-master/build/lib/lua/Release/lua51.lib")
    else()
        # Linux/macOS: Lumen 静态库名为 liblumen-static.a (未设置 OUTPUT_NAME)
        find_library(LUMEN_LIB_PATH
            NAMES lumen-static lua51 lua5.1 lua
            PATHS "${CMAKE_SOURCE_DIR}/../lumen-master/build/lib/lua"
            NO_DEFAULT_PATH
        )
        if(NOT LUMEN_LIB_PATH)
            # 回退: 让链接器在标准路径查找
            set(LUMEN_LIB_PATH "lua51")
        endif()
    endif()
endif()
message(STATUS "Lumen lib: ${LUMEN_LIB_PATH}")

# ==================== 源文件 ====================

add_library(Light SHARED
    src/cc_core.cpp
    src/light_module.cpp
    src/light_debug.cpp
    src/light_math.cpp
    src/light_data.cpp
    src/light_ui.cpp
    src/light_graphics.cpp
    src/light_graphics_canvas.cpp
    src/light_graphics_image.cpp
    src/light_graphics_font.cpp
    src/light_av.cpp
    src/light_db.cpp
    src/light_network.cpp
    src/light_record.cpp
    src/light_plugins.cpp
    src/light_antidebug.cpp
    # 平台窗口/事件抽象层 (SDL3)
    src/platform_window_sdl3.cpp
    # 渲染后端抽象层
    src/render_backend.cpp
    src/render_legacy.cpp
    src/render_gl33.cpp
    # 音频后端抽象层
    src/light_audio_backend.cpp
    # 网络抽象层 (libuv)
    src/light_platform_net.cpp
    # SQLite3 amalgamation (C 源码)
    ${SQLITE3_DIR}/sqlite3.c
    # stb 库实现 (stb_image + stb_truetype)
    ${CMAKE_SOURCE_DIR}/third_party/stb_impl.c
    # glad GL 3.3 Core Profile 加载器
    ${GLAD_DIR}/src/gl.c
    # miniaudio 音频库实现
    ${CMAKE_SOURCE_DIR}/third_party/miniaudio_impl.c
)

# ==================== 头文件搜索路径 ====================

target_include_directories(Light PRIVATE
    include
    ${LUA_INCLUDE_DIR}
    ${SQLITE3_DIR}
    ${GLAD_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/libuv/include
)

# ==================== 链接库 ====================

# 核心库: Lumen + SDL3 + libuv
target_link_libraries(Light PRIVATE
    ${LUMEN_LIB_PATH}
    SDL3::SDL3-static
    uv_a
)

# 平台系统库
if(WIN32)
    target_link_libraries(Light PRIVATE
        ws2_32 gdi32 user32 shell32 opengl32 winmm
    )
elseif(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    find_library(OPENGL_FRAMEWORK OpenGL)
    find_library(COREAUDIO_FRAMEWORK CoreAudio)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox)
    target_link_libraries(Light PRIVATE
        ${COCOA_FRAMEWORK} ${IOKIT_FRAMEWORK} ${COREVIDEO_FRAMEWORK}
        ${OPENGL_FRAMEWORK} ${COREAUDIO_FRAMEWORK} ${AUDIOTOOLBOX_FRAMEWORK}
        pthread dl
    )
else()
    # Linux
    find_package(OpenGL REQUIRED)
    target_link_libraries(Light PRIVATE
        OpenGL::GL pthread dl m
    )
endif()

# ==================== 编译定义 ====================

target_compile_definitions(Light PRIVATE LIGHT_EXPORTS SQLITE_THREADSAFE=1)

if(WIN32)
    target_compile_definitions(Light PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ==================== 输出配置 ====================

set_target_properties(Light PROPERTIES
    OUTPUT_NAME "Light"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ==================== 编译器选项 ====================

if(MSVC)
    target_compile_options(Light PRIVATE /utf-8 /W3)
else()
    target_compile_options(Light PRIVATE -Wall -Wno-unused-function -Wno-missing-field-initializers)
endif()
