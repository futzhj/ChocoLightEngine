cmake_minimum_required(VERSION 3.16)
project(ChocoLight LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Lua 5.1 SDK 路径
set(LUA_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../lumen-master/include")
set(LUA_LIB_DIR "${CMAKE_SOURCE_DIR}/../Light-0.2.3")

# SQLite3 amalgamation
set(SQLITE3_DIR "${CMAKE_SOURCE_DIR}/third_party/sqlite3")

# GLFW3 静态库 (窗口管理 + OpenGL context)
set(GLFW_DIR "${CMAKE_SOURCE_DIR}/third_party/glfw")

# 输出 Light.dll
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
    # SQLite3 amalgamation (C 源码)
    ${SQLITE3_DIR}/sqlite3.c
    # stb 库实现 (stb_image + stb_truetype)
    ${CMAKE_SOURCE_DIR}/third_party/stb_impl.c
)

target_include_directories(Light PRIVATE
    include
    ${LUA_INCLUDE_DIR}
    ${SQLITE3_DIR}
    ${GLFW_DIR}/include
    ${CMAKE_SOURCE_DIR}/third_party
)

# Lua 5.1 + GLFW3 链接
target_link_libraries(Light PRIVATE
    ${CMAKE_SOURCE_DIR}/../lumen-master/build/lib/lua/Release/lua51.lib
    ${GLFW_DIR}/glfw3.lib
)

# 系统库
target_link_libraries(Light PRIVATE
    ws2_32
    gdi32
    user32
    shell32
    opengl32
    winmm
)

# 导出定义 — 使用 .def 文件精确控制导出符号
set_target_properties(Light PROPERTIES
    OUTPUT_NAME "Light"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 导出宏
target_compile_definitions(Light PRIVATE LIGHT_EXPORTS _CRT_SECURE_NO_WARNINGS SQLITE_THREADSAFE=1)

# MSVC 设置
if(MSVC)
    target_compile_options(Light PRIVATE /utf-8 /W3)
    # x64 目标
    set(CMAKE_GENERATOR_PLATFORM x64)
endif()
