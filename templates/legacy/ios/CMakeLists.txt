cmake_minimum_required(VERSION 3.16)

# 在 project() 之前设置部署目标
set(CMAKE_OSX_DEPLOYMENT_TARGET "16.4" CACHE STRING "" FORCE)

project(ChocoLightIOS LANGUAGES C CXX OBJC)

set(CMAKE_CXX_STANDARD 17)

# 抑制 sprintf deprecated 警告和 availability 错误
add_compile_options(-Wno-deprecated-declarations -Wno-unguarded-availability-new)

# Lumen 源码目录（CI 中由上层复制或软链接）
set(LUMEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../lumen-master")

# 直接编译 Lumen 核心源码
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
    LUA_USE_IOS
)

# iOS 应用
add_executable(ChocoLight main.m)

target_include_directories(ChocoLight PRIVATE
    "${LUMEN_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
)

target_link_libraries(ChocoLight PRIVATE
    lumen-embedded
    "-framework UIKit"
    "-framework Foundation"
)

# Bundle main.lua 到 app 资源中
set(LUA_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/assets/main.lua")
if(EXISTS "${LUA_ASSETS}")
    target_sources(ChocoLight PRIVATE "${LUA_ASSETS}")
    set_source_files_properties("${LUA_ASSETS}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
endif()

set_target_properties(ChocoLight PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.chocolight.engine"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
)
