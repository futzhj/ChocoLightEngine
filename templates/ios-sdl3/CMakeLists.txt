cmake_minimum_required(VERSION 3.20)

# iOS 部署目标
# std::to_chars (Lumen VM) 需要 iOS 16.3+, 与旧模板保持一致用 16.4
set(CMAKE_OSX_DEPLOYMENT_TARGET "16.4" CACHE STRING "" FORCE)

project(ChocoLightIOS LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 抑制 deprecated 警告
add_compile_options(-Wno-deprecated-declarations -Wno-unguarded-availability-new)

# ==================== 路径配置 ====================
set(LUMEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../lumen-master")
set(CHOCOLIGHT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ChocoLight")

# ==================== Lumen (嵌入式静态库) ====================
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

target_compile_definitions(lumen-embedded PRIVATE LUA_USE_IOS LUA_USE_COMPAT)

# ==================== ChocoLight 引擎 ====================
set(LUMEN_LIB_PATH lumen-embedded CACHE STRING "" FORCE)
add_subdirectory(${CHOCOLIGHT_DIR} ${CMAKE_BINARY_DIR}/ChocoLight)

# ==================== iOS 应用 ====================
set(APP_SOURCES main.m)
set_source_files_properties(main.m PROPERTIES LANGUAGE OBJCXX)

# 资产文件
set(LUA_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/assets/main.lua")
set(LAUNCH_STORYBOARD "${CMAKE_CURRENT_SOURCE_DIR}/LaunchScreen.storyboard")

add_executable(ChocoLightApp ${APP_SOURCES})

target_include_directories(ChocoLightApp PRIVATE
    "${LUMEN_DIR}/include"
    "${CHOCOLIGHT_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
)

target_link_libraries(ChocoLightApp PRIVATE
    Light
    lumen-embedded
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

# Bundle 资源
if(EXISTS "${LUA_ASSETS}")
    target_sources(ChocoLightApp PRIVATE "${LUA_ASSETS}")
    set_source_files_properties("${LUA_ASSETS}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
endif()

if(EXISTS "${LAUNCH_STORYBOARD}")
    target_sources(ChocoLightApp PRIVATE "${LAUNCH_STORYBOARD}")
    set_source_files_properties("${LAUNCH_STORYBOARD}" PROPERTIES
        MACOSX_PACKAGE_LOCATION Resources)
endif()

set_target_properties(ChocoLightApp PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.chocolight.engine"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
)
