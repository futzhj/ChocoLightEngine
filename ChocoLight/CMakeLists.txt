cmake_minimum_required(VERSION 3.20)
project(ChocoLight LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CHOCO_IOS ON)
    enable_language(OBJC OBJCXX)
else()
    set(CHOCO_IOS OFF)
endif()

# ==================== SDL3 (FetchContent) ====================
# 从官方仓库拉取源码编译, 锁定稳定版 release-3.2.0
# 静态链接到 Light.dll/so/dylib, 避免运行时动态库依赖

include(FetchContent)

# Light 是 SHARED 库, 链接的静态依赖 (SDL3/libuv/Box2D) 必须带 -fPIC
# 否则 Linux ld 报 R_X86_64_PC32 relocation 错误
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# SDL3 编译选项: 只要 video+events+gl, 不要 audio (我们用 miniaudio)/test/examples
if(ANDROID)
    # Android: SDL3 必须为 SHARED (SDLActivity Java 通过 System.loadLibrary 加载)
    set(SDL_SHARED        ON  CACHE BOOL "" FORCE)
    set(SDL_STATIC        OFF CACHE BOOL "" FORCE)
else()
    set(SDL_SHARED        OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC        ON  CACHE BOOL "" FORCE)
endif()
set(SDL_TEST_LIBRARY  OFF CACHE BOOL "" FORCE)
set(SDL_TESTS         OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO         ON  CACHE BOOL "" FORCE)  # Phase AM: Light.Audio 模块需要; 与 miniaudio 共存 (各自后端)
set(SDL_RENDER        OFF CACHE BOOL "" FORCE)  # 我们不用 SDL_Renderer (用 GL)
set(SDL_GPU           OFF CACHE BOOL "" FORCE)  # 不用 SDL_GPU

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.30
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)

message(STATUS "SDL3: integrated via FetchContent (release-3.2.30)")

# ==================== Box2D v2.4.1 (vendored, non-Web) ====================
if(NOT EMSCRIPTEN)
    set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
    set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(BOX2D_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/box2d ${CMAKE_CURRENT_BINARY_DIR}/box2d EXCLUDE_FROM_ALL)
    message(STATUS "Box2D: integrated via vendored source (v2.4.1)")
else()
    message(STATUS "Box2D: disabled on Web; Physics exports stubbed")
endif()

# ==================== Phase AU — Bullet 3 (vendored, all platforms) ====================
# 保留 LinearMath + BulletCollision + BulletDynamics + BulletSoftBody 四个子库, 用 file GLOB 编译
# 不用 Bullet 自带的复杂 CMake (Demos/Extras 已删除, 自带 CMake 会报错)
# BulletSoftBody 仅引入"经典 SoftBody"必要文件 (rope/cloth/ellipsoid/anchor),
# 跳过 Deformable / ReducedDeformable / SoftMultiBody (不需要)
set(BULLET3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/bullet3")

file(GLOB_RECURSE BULLET_LM_SRC  "${BULLET3_DIR}/src/LinearMath/*.cpp")
file(GLOB_RECURSE BULLET_COL_SRC "${BULLET3_DIR}/src/BulletCollision/*.cpp")
file(GLOB_RECURSE BULLET_DYN_SRC "${BULLET3_DIR}/src/BulletDynamics/*.cpp")
file(GLOB         BULLET_SOFT_SRC "${BULLET3_DIR}/src/BulletSoftBody/*.cpp")

add_library(bullet3 STATIC
    ${BULLET_LM_SRC}
    ${BULLET_COL_SRC}
    ${BULLET_DYN_SRC}
    ${BULLET_SOFT_SRC}
)
target_include_directories(bullet3 PUBLIC "${BULLET3_DIR}/src")

# Web (Emscripten): Bullet SSE/AVX SIMD path 不能用
if(EMSCRIPTEN)
    target_compile_definitions(bullet3 PRIVATE BT_NO_SIMD_OPERATOR_OVERLOADS=1)
endif()
# btScalar 类型选择: btScalar.h 用 #if defined(BT_USE_DOUBLE_PRECISION) 而非
# #if BT_USE_DOUBLE_PRECISION, 所以 *绝对不能* 定义此宏 (即使值 = 0 也会触发 double 分支).
# 不定义 -> btScalar = float (与 light_physics3d.cpp 的 float* 数组兼容).
# Bullet 内部假设的几个宏在某些 NDK / Xcode 下需禁用
if(MSVC)
    target_compile_options(bullet3 PRIVATE /wd4244 /wd4267 /wd4305 /wd4316 /wd4267)
else()
    target_compile_options(bullet3 PRIVATE -Wno-deprecated-declarations -Wno-unused-but-set-variable -Wno-unused-variable)
endif()

message(STATUS "Bullet3: integrated via vendored source (v3.25, LinearMath + BulletCollision + BulletDynamics + BulletSoftBody)")

# ==================== 路径配置 ====================

# Lua 5.1 SDK 路径
set(LUA_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lumen-master/include")

# SQLite3 amalgamation
set(SQLITE3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite3")

# glad GL 3.3 Core Profile 加载器 (桌面专用, GLES3 平台不需要)
if(NOT EMSCRIPTEN AND NOT ANDROID AND NOT CHOCO_IOS)
    set(GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glad")
endif()

# libuv 异步 IO 库 (网络抽象, Emscripten/Android/iOS 不支持)
if(NOT EMSCRIPTEN AND NOT ANDROID AND NOT CHOCO_IOS)
    set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/libuv ${CMAKE_CURRENT_BINARY_DIR}/libuv EXCLUDE_FROM_ALL)
endif()

# ==================== Phase BC T6: cJSON (FetchContent) ====================
# cJSON v1.7.18 — JSON 解析器/构造器, MIT license
# 用于 Phase BC RPC + Room state 序列化
# 全平台启用 (含 Web), 因 cJSON 纯 C 无平台依赖
#
# 关键 option:
#   ENABLE_CUSTOM_COMPILER_FLAGS=OFF: 关闭 cJSON 自带 -Werror -Wconversion 等
#       (跨 GCC/Clang/MSVC 版本可能噪音, 我们用自己的 -Wall 规则)
#   CJSON_OVERRIDE_BUILD_SHARED_LIBS=ON + CJSON_BUILD_SHARED_LIBS=OFF: 强制 STATIC
#   ENABLE_TARGET_EXPORT=OFF: 不污染 install/export
#
# 兼容性修补: cJSON 1.7.18 的 CMakeLists.txt 用 cmake_minimum_required(VERSION 3.0),
# 在 CMake 4.0+ (macOS-latest runner) 被拒绝 ("Compatibility with CMake < 3.5
# has been removed from CMake"). 通过设置 CMAKE_POLICY_VERSION_MINIMUM=3.5
# 强制兼容 (CMake 3.24+ 才读此变量, 旧 CMake 静默忽略, 不影响其他平台).
set(CMAKE_POLICY_VERSION_MINIMUM    3.5 CACHE STRING "" FORCE)
set(ENABLE_CJSON_TEST              OFF CACHE BOOL   "" FORCE)
set(ENABLE_CJSON_UTILS             OFF CACHE BOOL   "" FORCE)
set(ENABLE_CUSTOM_COMPILER_FLAGS   OFF CACHE BOOL   "" FORCE)
set(CJSON_OVERRIDE_BUILD_SHARED_LIBS ON CACHE BOOL  "" FORCE)
set(CJSON_BUILD_SHARED_LIBS        OFF CACHE BOOL   "" FORCE)
set(ENABLE_TARGET_EXPORT           OFF CACHE BOOL   "" FORCE)

FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cjson)

# 修补: cJSON 上游不暴露 target_include_directories, 消费者拿不到 cJSON.h
target_include_directories(cjson INTERFACE
    $<BUILD_INTERFACE:${cjson_SOURCE_DIR}>
)

message(STATUS "cJSON: integrated via FetchContent (v1.7.18)")

# ==================== Phase BC T1: ENet (FetchContent) ====================
# ENet 1.3.18 — reliable UDP library, MIT license
# 桌面 + Android + iOS 使用 (ENet 基于 BSD socket, 跨平台原生支持)
# Web (Emscripten) 跳过 (浏览器无 raw UDP, 走 WebRTC 是另一套 API)
#
# 注: ENet 上游 CMakeLists.txt 写死 add_library(enet STATIC ...), 无 STATIC/SHARED
#     选项. 上游用 include_directories() 而非 target_include_directories(),
#     FetchContent 集成后消费者拿不到 include path -> 必须手动给 enet target
#     追加 INTERFACE include directory.
if(NOT EMSCRIPTEN)
    FetchContent_Declare(
        enet
        GIT_REPOSITORY https://github.com/lsalzman/enet.git
        GIT_TAG        v1.3.18
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(enet)

    # 修补: 让消费者能 #include "enet/enet.h"
    target_include_directories(enet INTERFACE
        $<BUILD_INTERFACE:${enet_SOURCE_DIR}/include>
    )

    message(STATUS "ENet: integrated via FetchContent (v1.3.18)")
endif()

# ==================== Lumen (lua51) 库路径 ====================

# 允许外部指定, 否则使用默认路径
if(NOT DEFINED LUMEN_LIB_PATH)
    if(WIN32)
        set(LUMEN_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lumen-master/build/lib/lua/Release")
        if(EXISTS "${LUMEN_LIB_DIR}/lua51.lib")
            set(LUMEN_LIB_PATH "${LUMEN_LIB_DIR}/lua51.lib")
        elseif(EXISTS "${LUMEN_LIB_DIR}/Lumen.lib")
            set(LUMEN_LIB_PATH "${LUMEN_LIB_DIR}/Lumen.lib")
        else()
            set(LUMEN_LIB_PATH "${LUMEN_LIB_DIR}/lua51.lib")
        endif()
    else()
        # Linux/macOS: Lumen 静态库名为 liblumen-static.a (未设置 OUTPUT_NAME)
        find_library(LUMEN_LIB_PATH
            NAMES lumen-static lua51 lua5.1 lua
            PATHS "${CMAKE_CURRENT_SOURCE_DIR}/../lumen-master/build/lib/lua"
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

# 通用源文件 (所有平台) — 使用绝对路径以支持 add_subdirectory 引入
set(CHOCO_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(LIGHT_SOURCES
    ${CHOCO_SRC}/cc_core.cpp
    ${CHOCO_SRC}/light_module.cpp
    ${CHOCO_SRC}/light_debug.cpp
    ${CHOCO_SRC}/light_math.cpp
    ${CHOCO_SRC}/light_data.cpp
    ${CHOCO_SRC}/light_ui.cpp
    ${CHOCO_SRC}/light_graphics.cpp
    ${CHOCO_SRC}/light_graphics_canvas.cpp
    ${CHOCO_SRC}/light_graphics_image.cpp
    ${CHOCO_SRC}/light_graphics_font.cpp
    ${CHOCO_SRC}/light_av.cpp
    ${CHOCO_SRC}/light_db.cpp
    ${CHOCO_SRC}/light_network.cpp
    ${CHOCO_SRC}/light_network_packet.cpp
    ${CHOCO_SRC}/light_network_udp.cpp
    ${CHOCO_SRC}/light_network_rpc.cpp
    ${CHOCO_SRC}/light_network_room.cpp
    ${CHOCO_SRC}/light_record.cpp
    ${CHOCO_SRC}/light_plugins.cpp
    ${CHOCO_SRC}/light_particles.cpp
    ${CHOCO_SRC}/light_tilemap.cpp
    ${CHOCO_SRC}/light_ecs.cpp
    ${CHOCO_SRC}/light_scene.cpp
    ${CHOCO_SRC}/light_graphics_spriteanimation.cpp
    ${CHOCO_SRC}/light_graphics_shader.cpp
    ${CHOCO_SRC}/light_hotreload.cpp
    ${CHOCO_SRC}/light_ui_widget.cpp
    ${CHOCO_SRC}/light_crypto.cpp
    ${CHOCO_SRC}/light_antidebug.cpp
    ${CHOCO_SRC}/light_io.cpp
    ${CHOCO_SRC}/light_storage.cpp
    ${CHOCO_SRC}/light_locale.cpp
    ${CHOCO_SRC}/light_power.cpp
    ${CHOCO_SRC}/light_sensor.cpp
    ${CHOCO_SRC}/light_process.cpp
    ${CHOCO_SRC}/light_camera.cpp
    ${CHOCO_SRC}/light_messagebox.cpp
    ${CHOCO_SRC}/light_clipboard.cpp
    ${CHOCO_SRC}/light_url.cpp
    ${CHOCO_SRC}/light_display.cpp
    ${CHOCO_SRC}/light_cursor.cpp
    ${CHOCO_SRC}/light_gamepad.cpp
    ${CHOCO_SRC}/light_guid.cpp
    ${CHOCO_SRC}/light_system.cpp
    ${CHOCO_SRC}/light_tray.cpp
    ${CHOCO_SRC}/light_hidapi.cpp
    ${CHOCO_SRC}/light_haptic.cpp
    ${CHOCO_SRC}/light_dialog.cpp
    ${CHOCO_SRC}/light_touch.cpp
    ${CHOCO_SRC}/light_hints.cpp
    ${CHOCO_SRC}/light_time.cpp
    ${CHOCO_SRC}/light_event.cpp
    ${CHOCO_SRC}/light_graphics_mesh.cpp
    ${CHOCO_SRC}/light_graphics_material.cpp
    ${CHOCO_SRC}/light_endian.cpp
    ${CHOCO_SRC}/light_log.cpp
    ${CHOCO_SRC}/light_pixels.cpp
    ${CHOCO_SRC}/light_blendmode.cpp
    ${CHOCO_SRC}/light_filesystem.cpp
    ${CHOCO_SRC}/light_cpuinfo.cpp
    ${CHOCO_SRC}/light_error.cpp
    ${CHOCO_SRC}/light_loadso.cpp
    ${CHOCO_SRC}/light_version.cpp
    ${CHOCO_SRC}/light_properties.cpp
    ${CHOCO_SRC}/light_misc.cpp
    ${CHOCO_SRC}/light_rect.cpp
    ${CHOCO_SRC}/light_atomic.cpp
    ${CHOCO_SRC}/light_mutex.cpp
    ${CHOCO_SRC}/light_iostream.cpp
    ${CHOCO_SRC}/light_surface.cpp
    ${CHOCO_SRC}/light_keyboard.cpp
    ${CHOCO_SRC}/light_mouse.cpp
    ${CHOCO_SRC}/light_joystick.cpp
    ${CHOCO_SRC}/light_audio.cpp
    ${CHOCO_SRC}/platform_window_sdl3.cpp
    ${CHOCO_SRC}/render_backend.cpp
    ${CHOCO_SRC}/batch_renderer.cpp
    ${CHOCO_SRC}/lit_batch_renderer.cpp
    ${CHOCO_SRC}/hdr_renderer.cpp
    ${CHOCO_SRC}/render_gl33.cpp
    ${CHOCO_SRC}/light_audio_backend.cpp
    # Phase AT — Audio 3D 空间化 + Mixer + Effects
    ${CHOCO_SRC}/light_audio_sound.cpp
    ${CHOCO_SRC}/light_audio_group.cpp
    ${CHOCO_SRC}/light_audio_effect.cpp
    ${CHOCO_SRC}/light_platform_net.cpp
    ${SQLITE3_DIR}/sqlite3.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio_impl.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tiny_aes.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tiny_hash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgltf_impl.c
)

# 平台条件源文件
if(EMSCRIPTEN)
    # Web: HTML5 视频后端 + 浏览器原生网络, 无 Legacy GL, 无 glad
    list(APPEND LIGHT_SOURCES
        ${CHOCO_SRC}/video_backend_html5.cpp
        ${CHOCO_SRC}/light_network_web.cpp
    )
elseif(ANDROID)
    # Android: MediaPlayer + SurfaceTexture → OES → TEXTURE_2D
    # POSIX socket 网络, 无 Legacy GL, 无 glad
    list(APPEND LIGHT_SOURCES
        ${CHOCO_SRC}/video_backend_mediaPlayer.cpp
        ${CHOCO_SRC}/light_platform_net_mobile.cpp
    )
elseif(CHOCO_IOS)
    # iOS: AVPlayer 视频 + POSIX socket 网络, 无 Legacy GL, 无 glad
    list(APPEND LIGHT_SOURCES
        ${CHOCO_SRC}/video_backend_avplayer.mm
        ${CHOCO_SRC}/light_platform_net_mobile.cpp
    )
else()
    # 桌面: FFmpeg 视频后端 + Legacy GL + glad
    list(APPEND LIGHT_SOURCES
        ${CHOCO_SRC}/video_backend_ffmpeg.cpp
        ${CHOCO_SRC}/render_legacy.cpp
        ${GLAD_DIR}/src/gl.c
    )
endif()

# Phase BC T4: ENet 实现 (桌面 + 移动, Web 跳过)
# 文件内部已用 #if !defined(__EMSCRIPTEN__) guard, CMake 条件排除避免空编译开销
if(NOT EMSCRIPTEN)
    list(APPEND LIGHT_SOURCES ${CHOCO_SRC}/light_platform_net_enet.cpp)
endif()

# light_input.cpp 内部用 #ifdef __ANDROID__ 提供空壳, 可全平台编译
list(APPEND LIGHT_SOURCES ${CHOCO_SRC}/light_input.cpp)

# Physics 源文件: 非 Web 使用 Box2D, Web 保留导出空壳
list(APPEND LIGHT_SOURCES ${CHOCO_SRC}/light_physics.cpp)

# Phase AU — Light.Physics3D (基于 Bullet 3, 全平台含 Web)
list(APPEND LIGHT_SOURCES ${CHOCO_SRC}/light_physics3d.cpp)

# Phase AV — Light.Animation (骨骼动画 + 状态机, 复用 cgltf, 0 新依赖)
list(APPEND LIGHT_SOURCES ${CHOCO_SRC}/light_animation.cpp)

# Phase E.1.3 — Light.Lighting2D (2D forward 多光状态管理, 0 新依赖)
list(APPEND LIGHT_SOURCES ${CHOCO_SRC}/light_lighting2d.cpp)

# Emscripten/Android: STATIC 库 (链接到最终二进制)
# 桌面: SHARED 库 (Light.dll/so/dylib)
if(EMSCRIPTEN OR ANDROID OR CHOCO_IOS)
    add_library(Light STATIC ${LIGHT_SOURCES})
else()
    add_library(Light SHARED ${LIGHT_SOURCES})
endif()

# ==================== 头文件搜索路径 ====================

target_include_directories(Light PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LUA_INCLUDE_DIR}
    ${SQLITE3_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if(NOT EMSCRIPTEN AND NOT ANDROID AND NOT CHOCO_IOS)
    target_include_directories(Light PRIVATE
        ${GLAD_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libuv/include
    )
endif()

# ==================== 链接库 ====================

if(EMSCRIPTEN)
    # Web: Lumen + SDL3 static, 无 libuv
    target_link_libraries(Light PRIVATE
        ${LUMEN_LIB_PATH}
        SDL3::SDL3-static
    )
elseif(ANDROID)
    # Android: Lumen + SDL3 shared + GLES3, 无 libuv
    target_link_libraries(Light PRIVATE
        ${LUMEN_LIB_PATH}
        SDL3::SDL3
        GLESv3 EGL android log
    )
elseif(CHOCO_IOS)
    # iOS: Lumen + SDL3 static + OpenGLES, 无 libuv
    target_link_libraries(Light PRIVATE
        ${LUMEN_LIB_PATH}
        SDL3::SDL3-static
        "-framework UIKit"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework CoreVideo"
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework OpenGLES"
    )
else()
    # 桌面: Lumen + SDL3 static + libuv
    target_link_libraries(Light PRIVATE
        ${LUMEN_LIB_PATH}
        SDL3::SDL3-static
        uv_a
    )
endif()

# 平台系统库
if(WIN32)
    target_link_libraries(Light PRIVATE
        ws2_32 gdi32 user32 shell32 opengl32 winmm
    )
elseif(APPLE AND NOT EMSCRIPTEN AND NOT CHOCO_IOS)
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
elseif(NOT EMSCRIPTEN AND NOT ANDROID AND NOT CHOCO_IOS)
    # Linux
    find_package(OpenGL REQUIRED)
    target_link_libraries(Light PRIVATE
        OpenGL::GL pthread dl m
    )
endif()

# Box2D: desktop/mobile enabled, Web stubbed
if(NOT EMSCRIPTEN)
    target_link_libraries(Light PRIVATE box2d)
    target_compile_definitions(Light PRIVATE CHOCO_HAS_BOX2D=1)
else()
    target_compile_definitions(Light PRIVATE CHOCO_HAS_BOX2D=0)
endif()

# Phase AU — Bullet 3 全平台 link (含 Web), 用于 Light.Physics3D
target_link_libraries(Light PRIVATE bullet3)
target_compile_definitions(Light PRIVATE CHOCO_HAS_BULLET=1)

# Phase BC T1 — ENet reliable UDP, 桌面 + 移动, Web 跳过 (浏览器无 raw UDP)
if(NOT EMSCRIPTEN)
    target_link_libraries(Light PRIVATE enet)
    target_compile_definitions(Light PRIVATE CHOCO_NET_ENET_ENABLED=1)
    # ENet 在 Windows 需要 winsock2 (已由 Light 自身链接 ws2_32), 其他平台 ENet 自管
else()
    target_compile_definitions(Light PRIVATE CHOCO_NET_ENET_ENABLED=0)
endif()

# Phase BC T6 — cJSON (RPC + Room 序列化, 全平台启用)
target_link_libraries(Light PRIVATE cjson)

# ==================== 编译定义 ====================

target_compile_definitions(Light PRIVATE LIGHT_EXPORTS SQLITE_THREADSAFE=1)

if(WIN32)
    target_compile_definitions(Light PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if(CHOCO_IOS)
    target_compile_definitions(Light PRIVATE CHOCO_PLATFORM_IOS=1)
    # miniaudio 在 iOS 上 include <AVFoundation/AVFoundation.h> (ObjC),
    # .c 文件无法解析 @class/@protocol, 必须以 ObjC 编译
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio_impl.c
        PROPERTIES LANGUAGE OBJC
    )
    # stb_impl.c 同理, 虽然不含 ObjC 头但统一处理避免潜在问题
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb_impl.c
        PROPERTIES LANGUAGE OBJC
    )
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
elseif(EMSCRIPTEN)
    # Emscripten: WebGL2 + GLES3, 允许内存增长
    target_compile_options(Light PRIVATE
        -Wall -Wno-unused-function -Wno-missing-field-initializers
        "SHELL:-s USE_SDL=0"  # 不使用 Emscripten ports 的 SDL, 我们用 FetchContent
    )
    target_link_options(Light PRIVATE
        "SHELL:-s USE_WEBGL2=1"
        "SHELL:-s FULL_ES3=1"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s FETCH=1"
        "SHELL:-s EXPORTED_FUNCTIONS=['_main']"
        "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
    )
else()
    target_compile_options(Light PRIVATE -Wall -Wno-unused-function -Wno-missing-field-initializers)
endif()
