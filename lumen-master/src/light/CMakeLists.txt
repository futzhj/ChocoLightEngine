# ----- Embed Lua script into exe (optional) -----
# Usage: cmake -DLIGHT_EMBED_SCRIPT=path/to/main.lua ...
# The script will be auto-converted to a C header on every build.
set(LIGHT_EMBED_SCRIPT "" CACHE FILEPATH "Lua script to embed into light/lightw exe")
set(EMBEDDED_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/embedded_scripts.h")
set(LUA2HEADER_TOOL "${LUMEN_BASE_SOURCE_DIR}/tools/lua2header.py")

if (LIGHT_EMBED_SCRIPT AND EXISTS "${LIGHT_EMBED_SCRIPT}")
    # Auto-run lua2header.py before compiling light.cpp
    add_custom_command(
        OUTPUT "${EMBEDDED_HEADER}"
        COMMAND ${CMAKE_COMMAND} -E echo "[lua2header] Embedding ${LIGHT_EMBED_SCRIPT}"
        COMMAND python "${LUA2HEADER_TOOL}" "${EMBEDDED_HEADER}" "${LIGHT_EMBED_SCRIPT}"
        DEPENDS "${LIGHT_EMBED_SCRIPT}" "${LUA2HEADER_TOOL}"
        COMMENT "Converting Lua script to embedded header"
    )
    # Force light.cpp to recompile when the header changes
    set_source_files_properties(light.cpp PROPERTIES OBJECT_DEPENDS "${EMBEDDED_HEADER}")
endif ()

if (CMAKE_SYSTEM_NAME MATCHES "Windows")
    set(LUA_PRODUCT_RESOURCE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
    configure_file(
        version.rc.in
        "${LUA_PRODUCT_RESOURCE}"
        @ONLY
    )
endif ()

add_executable(light light.cpp ${EMBEDDED_HEADER} ${LUA_PRODUCT_RESOURCE})
target_compile_definitions(light PRIVATE ${LUA_COMPILE_DEF})
target_compile_options(light INTERFACE ${LUA_COMPILE_OPT})
target_link_libraries(light PRIVATE Lumen ${LUA_LINK_LIBS})

if (CMAKE_SYSTEM_NAME MATCHES "Windows")
    add_executable(lightw light.cpp ${LUA_PRODUCT_RESOURCE})
    target_compile_definitions(lightw PRIVATE ${LUA_COMPILE_DEF})
    target_compile_options(lightw INTERFACE ${LUA_COMPILE_OPT})
    target_link_libraries(lightw PRIVATE Lumen ${LUA_LINK_LIBS})
    target_link_options(lightw PUBLIC "/SUBSYSTEM:WINDOWS" "/ENTRY:mainCRTStartup")

    add_custom_command(
        TARGET light POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Lumen> $<TARGET_FILE_DIR:light>
    )
    add_custom_command(
        TARGET lightw POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Lumen> $<TARGET_FILE_DIR:lightw>
    )
endif ()

if (NOT LUA_SKIP_INSTALL)
    install(
        TARGETS light
        DESTINATION bin
    )
    if (CMAKE_SYSTEM_NAME MATCHES "Windows")
        install(
            TARGETS lightw
            DESTINATION bin
        )
    elseif (CMAKE_SYSTEM_NAME MATCHES "Darwin")
        install(CODE "
            execute_process(
                COMMAND install_name_tool -add_rpath \"@executable_path/../lib\" \"\${CMAKE_INSTALL_PREFIX}/bin/light\"
            )
        ")
    elseif (CMAKE_SYSTEM_NAME MATCHES "Linux" OR CMAKE_SYSTEM_NAME MATCHES "Android")
        install(CODE "
            execute_process(
                COMMAND patchelf --set-rpath \"\\\$ORIGIN/../lib\" \"\${CMAKE_INSTALL_PREFIX}/bin/light\"
                RESULT_VARIABLE patch_result
            )
            if(NOT \${patch_result} EQUAL 0)
                message(WARNING \"patchelf command failed, RPATH might not be set correctly\")
            endif()
        ")
    endif ()
    install(
        FILES
        ${LUMEN_BASE_SOURCE_DIR}/doc/light.1
        DESTINATION share/man/man1
    )
endif ()