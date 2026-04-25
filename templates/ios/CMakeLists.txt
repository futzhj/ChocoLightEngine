cmake_minimum_required(VERSION 3.16)
project(ChocoLightIOS LANGUAGES C CXX OBJC)

set(CMAKE_CXX_STANDARD 17)

# Lumen 静态库（由 CI 预编译并复制到此目录）
set(LUMEN_LIBS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs")

add_executable(ChocoLight
    main.m
)

# 链接预编译的 Lumen 静态库
target_link_libraries(ChocoLight PRIVATE
    "${LUMEN_LIBS_DIR}/liblumen-static.a"
    "-framework UIKit"
    "-framework Foundation"
    "-framework OpenGLES"
    "-framework QuartzCore"
)

set_target_properties(ChocoLight PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.chocolight.engine"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
)
