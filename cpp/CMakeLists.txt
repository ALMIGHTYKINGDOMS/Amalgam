cmake_minimum_required(VERSION 3.21)
project(amalgam LANGUAGES CXX)

include(CTest)

file(GLOB AMALGAM_WINSQLITE_INCLUDE_CANDIDATES
    "C:/Program Files (x86)/Windows Kits/10/Include/*/um/winsqlite")
file(GLOB AMALGAM_WINSQLITE_LIBRARY_CANDIDATES
    "C:/Program Files (x86)/Windows Kits/10/Lib/*/um/x64/winsqlite3.lib")
list(SORT AMALGAM_WINSQLITE_INCLUDE_CANDIDATES)
list(SORT AMALGAM_WINSQLITE_LIBRARY_CANDIDATES)
list(REVERSE AMALGAM_WINSQLITE_INCLUDE_CANDIDATES)
list(REVERSE AMALGAM_WINSQLITE_LIBRARY_CANDIDATES)
if(AMALGAM_WINSQLITE_INCLUDE_CANDIDATES)
    list(GET AMALGAM_WINSQLITE_INCLUDE_CANDIDATES 0 AMALGAM_WINSQLITE_INCLUDE)
endif()
if(AMALGAM_WINSQLITE_LIBRARY_CANDIDATES)
    list(GET AMALGAM_WINSQLITE_LIBRARY_CANDIDATES 0 AMALGAM_WINSQLITE_LIBRARY)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Update-manifest verification key. When configured with a PEM file, the
# launcher embeds the public key and verifies signed update manifests.
# Without it the key is empty and signed manifests fail closed (updater.h).
set(AMALGAM_UPDATER_PUBLIC_KEY_FILE "" CACHE FILEPATH
    "PEM file with the update-manifest signing public key to embed")
set(AMALGAM_UPDATER_PUBLIC_KEY_PEM "")
if(AMALGAM_UPDATER_PUBLIC_KEY_FILE)
    if(EXISTS "${AMALGAM_UPDATER_PUBLIC_KEY_FILE}")
        file(READ "${AMALGAM_UPDATER_PUBLIC_KEY_FILE}" AMALGAM_UPDATER_PUBLIC_KEY_PEM)
        # Keep the embedded PEM byte-identical to the vault file (LF endings).
        string(REPLACE "\r\n" "\n" AMALGAM_UPDATER_PUBLIC_KEY_PEM "${AMALGAM_UPDATER_PUBLIC_KEY_PEM}")
    else()
        message(FATAL_ERROR
            "AMALGAM_UPDATER_PUBLIC_KEY_FILE does not exist: ${AMALGAM_UPDATER_PUBLIC_KEY_FILE}")
    endif()
endif()
set(UPDATER_PUBLIC_KEY_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated/updater_public_key.h")
configure_file(launcher/src/updater_public_key.h.in "${UPDATER_PUBLIC_KEY_GENERATED}" @ONLY NEWLINE_STYLE UNIX)

# Visual Studio builds individual test targets in parallel. /FS serializes
# access to shared debug-symbol files so a clean MSBuild build is as reliable
# as the Ninja build used by local release verification.
if(MSVC)
    # ImGui consumes UTF-8. Keep both the source and execution character sets
    # in UTF-8 so typographic punctuation, translated catalog metadata, and
    # non-English UI strings never become Windows-code-page replacement bytes.
    add_compile_options(/FS /utf-8)
endif()

# Shipping a native launcher and injected DLL demands more than a successful
# default build. Keep the quality baseline opt-out only for emergency legacy
# toolchains; normal local and CI builds use the same strict, hardened flags.
option(AMALGAM_STRICT_HARDENING "Enable compiler and linker hardening for shipped native targets" ON)
option(AMALGAM_ENABLE_ASAN "Enable AddressSanitizer (MSVC /fsanitize=address or Clang -fsanitize=address)" OFF)
option(AMALGAM_ENABLE_CLANG_TIDY "Enable clang-tidy static analysis if found" OFF)

function(amalgam_harden_native_target target)
    if(NOT AMALGAM_STRICT_HARDENING OR NOT MSVC)
        return()
    endif()
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc /sdl /guard:cf>)
    target_link_options(${target} PRIVATE /DYNAMICBASE /NXCOMPAT /guard:cf)
    # Whole-program optimization + link-time code generation for Release.
    # /GL enables WPO at compile time; /LTCG merges it at link time.
    # /OPT:REF strips unused functions; /OPT:ICF folds identical functions.
    # Together these cut 10-20% off binary size and improve runtime speed.
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/GL>)
    target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>)
    if(AMALGAM_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE /fsanitize=address)
        target_link_options(${target} PRIVATE /fsanitize=address)
    endif()
endfunction()

if(AMALGAM_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE})
        message(STATUS "clang-tidy found: ${CLANG_TIDY_EXE}")
    else()
        message(WARNING "AMALGAM_ENABLE_CLANG_TIDY is ON but clang-tidy was not found")
    endif()
endif()

if(NOT DEFINED AMALGAM_JAVA_HOME)
    # Prefer the project-specific environment variable used by the documented
    # build commands, then fall back to the conventional JAVA_HOME.
    set(AMALGAM_JAVA_HOME "$ENV{AMALGAM_JAVA_HOME}")
    if(NOT AMALGAM_JAVA_HOME)
        set(AMALGAM_JAVA_HOME "$ENV{JAVA_HOME}")
    endif()
endif()
if(NOT AMALGAM_JAVA_HOME)
    message(FATAL_ERROR "AMALGAM_JAVA_HOME (CMake or environment) or JAVA_HOME must point to a JDK with JNI headers")
endif()
if(NOT EXISTS "${AMALGAM_JAVA_HOME}/include/jni.h")
    message(FATAL_ERROR "JNI headers not found under AMALGAM_JAVA_HOME: ${AMALGAM_JAVA_HOME}")
endif()

add_subdirectory(vendor/minhook)

# libdatachannel supplies the native WebRTC data-channel transport used by
# Essentials. Build it statically so the release package does not need a
# second set of runtime DLLs. Media and WebSocket support are not needed by
# the TCP bridge.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "" FORCE)
set(NO_MEDIA ON CACHE BOOL "" FORCE)
set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
set(NO_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory(vendor/libdatachannel EXCLUDE_FROM_ALL)

set(AMALGAM_IMGUI_SOURCES
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_opengl3.cpp
)

add_library(amalgam SHARED
    src/dllmain.cpp
    src/bridge/bridge.cpp
    src/core/pipeline.cpp
    src/core/modules.cpp
    src/core/capabilities.cpp
    src/core/camera.cpp
    src/core/esp.cpp
    src/core/tracker.cpp
    src/core/game_mode_telemetry.cpp
    src/core/player_stats.cpp
    src/core/replay.cpp
    src/core/diagnostics_outbox.cpp
    src/modules/core_modules.cpp
    src/render/imgui_layer.cpp
    src/render/hud.cpp
    src/render/input.cpp
    src/client/client_core.cpp
    src/client/client_ui.cpp
    src/client/client_hud2.cpp
    src/client/client_input.cpp
    src/client/client_notifications.cpp
    ${AMALGAM_IMGUI_SOURCES}
)

target_include_directories(amalgam PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends
    ${AMALGAM_JAVA_HOME}/include
    ${AMALGAM_JAVA_HOME}/include/win32
)
if(AMALGAM_WINSQLITE_INCLUDE)
    target_include_directories(amalgam PRIVATE ${AMALGAM_WINSQLITE_INCLUDE}/..)
endif()

target_compile_definitions(amalgam PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(amalgam PRIVATE
    minhook
    opengl32
    ole32
    windowscodecs
    ws2_32
)
if(AMALGAM_WINSQLITE_LIBRARY)
    target_link_libraries(amalgam PRIVATE ${AMALGAM_WINSQLITE_LIBRARY})
endif()

set_target_properties(amalgam PROPERTIES
    OUTPUT_NAME amalgam
    PREFIX ""
)
amalgam_harden_native_target(amalgam)

option(AMALGAM_BUILD_LAUNCHER "Build the hybrid launcher" ON)
option(AMALGAM_DEVELOPER_UI "Expose publisher/developer-only launcher settings" OFF)
if(AMALGAM_BUILD_LAUNCHER)
     add_executable(amalgam_launcher WIN32
         launcher/src/main.cpp
         launcher/src/ai_ui.cpp
         launcher/src/ai_install.cpp
         launcher/src/ai_install_ui.cpp
         launcher/amalgam_launcher.rc
         launcher/src/json.cpp
         launcher/src/net.cpp
         launcher/src/model.cpp
         launcher/src/launch.cpp
         launcher/src/natives.cpp
         launcher/src/maven_artifact.cpp
         launcher/src/official_launcher_bridge.cpp
         launcher/src/official_launcher_guard.cpp
         launcher/src/java.cpp
         launcher/src/extract.cpp
         launcher/src/config.cpp
         launcher/src/admin_auth.cpp
         launcher/src/mods.cpp
         launcher/src/performance.cpp
         launcher/src/performance_cache.cpp
         launcher/src/ai.cpp
         ai/ai_core.cpp
         ai/ai_project_index.cpp
         ai/ai_checkpoint.cpp
         launcher/src/auth.cpp
          launcher/src/bedrock.cpp
          launcher/src/crash_report.cpp
         launcher/src/readiness.cpp
         launcher/src/instances.cpp
         launcher/src/profile_handoff.cpp
         launcher/src/import_pack.cpp
         launcher/src/ui_helpers.cpp
         launcher/src/ui_motion.cpp
         launcher/src/ui_components.cpp
         launcher/src/ui_shell.cpp
         launcher/src/ui.cpp
         launcher/src/supabase.cpp
         launcher/src/updater.cpp
          launcher/src/services.cpp
          launcher/src/essentials_manager.cpp
          launcher/src/server_manager.cpp
         launcher/src/server_providers.cpp
         launcher/src/server_provision.cpp
         launcher/src/storage_manager.cpp
         launcher/src/sync_manager.cpp
         launcher/src/account_manager.cpp
         launcher/src/auth_wizard.cpp
         launcher/src/account_ui.cpp
         launcher/src/server_ui.cpp
         launcher/src/cloud_provider.cpp
         launcher/src/cloud_ui.cpp
         launcher/src/online_config.cpp
         launcher/src/entitlements.cpp
         launcher/src/bedrock_ui.cpp
         launcher/src/admin_ui.cpp
         launcher/src/social_ui.cpp
           launcher/src/essentials_sync.cpp
           launcher/src/essentials_session.cpp
           launcher/src/essentials_tcp_bridge.cpp
          launcher/src/essentials_address.cpp
           launcher/src/essentials_ui.cpp
         launcher/src/mod_manager_ui.cpp
         launcher/src/performance_ui.cpp
         launcher/src/theme_ui.cpp
          launcher/src/loading_screen.cpp
          launcher/src/project_publishing.cpp
          launcher/src/diagnostics.cpp
         vendor/imgui/backends/imgui_impl_win32.cpp
         vendor/imgui/misc/cpp/imgui_stdlib.cpp
         ${AMALGAM_IMGUI_SOURCES}
     )
    target_include_directories(amalgam_launcher PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui
         ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends
         ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libdatachannel/include
         ${CMAKE_CURRENT_SOURCE_DIR}/ai
    )
    target_compile_definitions(amalgam_launcher PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        UNICODE
        _UNICODE
        $<$<BOOL:${AMALGAM_DEVELOPER_UI}>:AMALGAM_DEVELOPER_UI=1>
    )
         target_link_libraries(amalgam_launcher PRIVATE
        opengl32
        user32
        gdi32
        wininet
        shell32
        ole32
        windowscodecs
        comdlg32
        crypt32
         bcrypt
         dbghelp
         datachannel-static
         ws2_32
         iphlpapi
     )
    set_target_properties(amalgam_launcher PROPERTIES
        OUTPUT_NAME amalgam_launcher
        # CMake feeds this into the embedded Windows manifest, so the launcher
        # receives physical pixels instead of Windows bitmap virtualization.
        # It is the durable counterpart to the runtime fallback in ui.cpp.
        VS_DPI_AWARE "PerMonitor")
    amalgam_harden_native_target(amalgam_launcher)
    # A launcher build must be runnable directly from every build output, not
    # only from a hand-assembled release folder. Keep the real branded artwork
    # beside the executable so compact and full sidebar layouts never fall
    # back to the temporary vector mark.
    add_custom_command(TARGET amalgam_launcher POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:amalgam_launcher>/branding"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/../docs/branding"
                "$<TARGET_FILE_DIR:amalgam_launcher>/branding"
        VERBATIM)
    # The Ninja/MSVC generator does not apply VS_DPI_AWARE itself. Merge the
    # same declaration into the standard embedded manifest after linking.
    if(MSVC AND CMAKE_MT)
        set_property(TARGET amalgam_launcher APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/launcher/amalgam_launcher.manifest")
        add_custom_command(TARGET amalgam_launcher POST_BUILD
            COMMAND "${CMAKE_MT}" -nologo -manifest
                    "${CMAKE_CURRENT_SOURCE_DIR}/launcher/amalgam_launcher.manifest"
                    "-outputresource:$<TARGET_FILE:amalgam_launcher>$<SEMICOLON>#1"
            VERBATIM)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(amalgam_protocol_test tests/protocol_test.cpp)
    target_include_directories(amalgam_protocol_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME amalgam_protocol_test COMMAND amalgam_protocol_test)

    add_executable(amalgam_camera_test
        tests/camera_test.cpp
        src/core/camera.cpp)
    target_include_directories(amalgam_camera_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME amalgam_camera_test COMMAND amalgam_camera_test)

    add_executable(amalgam_tracker_test tests/tracker_test.cpp src/core/tracker.cpp)
    target_include_directories(amalgam_tracker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(amalgam_tracker_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_test(NAME amalgam_tracker_test COMMAND amalgam_tracker_test)

    add_executable(amalgam_game_mode_telemetry_test
        tests/game_mode_telemetry_test.cpp
        src/core/game_mode_telemetry.cpp
        src/core/replay.cpp
        src/core/diagnostics_outbox.cpp)
    target_include_directories(amalgam_game_mode_telemetry_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(AMALGAM_WINSQLITE_INCLUDE)
        target_include_directories(amalgam_game_mode_telemetry_test PRIVATE ${AMALGAM_WINSQLITE_INCLUDE}/..)
    endif()
    if(AMALGAM_WINSQLITE_LIBRARY)
        target_link_libraries(amalgam_game_mode_telemetry_test PRIVATE ${AMALGAM_WINSQLITE_LIBRARY})
    endif()
    add_test(NAME amalgam_game_mode_telemetry_test COMMAND amalgam_game_mode_telemetry_test)

    add_executable(amalgam_replay_test
        tests/replay_test.cpp
        src/core/replay.cpp
        src/core/game_mode_telemetry.cpp
        src/core/diagnostics_outbox.cpp)
    target_include_directories(amalgam_replay_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(AMALGAM_WINSQLITE_INCLUDE)
        target_include_directories(amalgam_replay_test PRIVATE ${AMALGAM_WINSQLITE_INCLUDE}/..)
    endif()
    if(AMALGAM_WINSQLITE_LIBRARY)
        target_link_libraries(amalgam_replay_test PRIVATE ${AMALGAM_WINSQLITE_LIBRARY})
    endif()
    add_test(NAME amalgam_replay_test COMMAND amalgam_replay_test)

    add_executable(amalgam_diagnostics_outbox_test
        tests/diagnostics_outbox_test.cpp
        src/core/game_mode_telemetry.cpp
        src/core/replay.cpp
        src/core/diagnostics_outbox.cpp)
    target_include_directories(amalgam_diagnostics_outbox_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    if(AMALGAM_WINSQLITE_INCLUDE)
        target_include_directories(amalgam_diagnostics_outbox_test PRIVATE ${AMALGAM_WINSQLITE_INCLUDE}/..)
    endif()
    if(AMALGAM_WINSQLITE_LIBRARY)
        target_link_libraries(amalgam_diagnostics_outbox_test PRIVATE ${AMALGAM_WINSQLITE_LIBRARY})
    endif()
    add_test(NAME amalgam_diagnostics_outbox_test COMMAND amalgam_diagnostics_outbox_test)

    add_executable(amalgam_module_test
        tests/module_test.cpp
        src/modules/core_modules.cpp
        src/core/modules.cpp
        src/core/pipeline.cpp
        src/render/input.cpp)
    target_include_directories(amalgam_module_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(amalgam_module_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(amalgam_module_test PRIVATE user32)
    add_test(NAME amalgam_module_test COMMAND amalgam_module_test)

    add_executable(amalgam_mods_test
        tests/mods_test.cpp
        launcher/src/mods.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_mods_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_mods_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(amalgam_mods_test PRIVATE wininet advapi32)
    add_test(NAME amalgam_mods_test COMMAND amalgam_mods_test)

    add_executable(amalgam_ai_test
        tests/ai_test.cpp         launcher/src/ai.cpp
         ai/ai_core.cpp
         ai/ai_project_index.cpp
         ai/ai_checkpoint.cpp
         launcher/src/json.cpp
         launcher/src/net.cpp)
    target_include_directories(amalgam_ai_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src ${CMAKE_CURRENT_SOURCE_DIR}/ai)
    target_compile_definitions(amalgam_ai_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(amalgam_ai_test PRIVATE wininet advapi32 bcrypt)
    add_test(NAME amalgam_ai_test COMMAND amalgam_ai_test)

    add_executable(amalgam_import_pack_test
        tests/import_pack_test.cpp
        launcher/src/import_pack.cpp
        launcher/src/mods.cpp
        launcher/src/instances.cpp
        launcher/src/extract.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_import_pack_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_import_pack_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(amalgam_import_pack_test PRIVATE wininet advapi32)
    add_test(NAME amalgam_import_pack_test COMMAND amalgam_import_pack_test)

    add_executable(amalgam_json_test tests/json_test.cpp launcher/src/json.cpp)
    target_include_directories(amalgam_json_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_json_test PRIVATE UNICODE _UNICODE)
    add_test(NAME amalgam_json_test COMMAND amalgam_json_test)

    add_executable(amalgam_json_atomic_test tests/json_atomic_test.cpp launcher/src/json.cpp)
    target_include_directories(amalgam_json_atomic_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_json_atomic_test PRIVATE UNICODE _UNICODE)
    add_test(NAME amalgam_json_atomic_test COMMAND amalgam_json_atomic_test)

    add_executable(amalgam_updater_test
        tests/updater_test.cpp
        launcher/src/updater.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp
        launcher/src/extract.cpp)
    target_include_directories(amalgam_updater_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_updater_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(amalgam_updater_test PRIVATE wininet crypt32)
    add_test(NAME amalgam_updater_test COMMAND amalgam_updater_test)

    add_executable(amalgam_config_test
        tests/config_test.cpp
        launcher/src/config.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_config_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_config_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(amalgam_config_test PRIVATE wininet advapi32 crypt32)
    add_test(NAME amalgam_config_test COMMAND amalgam_config_test)

    add_executable(amalgam_admin_auth_test
        tests/admin_auth_test.cpp
        launcher/src/admin_auth.cpp)
    target_include_directories(amalgam_admin_auth_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_admin_auth_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(amalgam_admin_auth_test PRIVATE bcrypt)
    add_test(NAME amalgam_admin_auth_test COMMAND amalgam_admin_auth_test)

    add_executable(amalgam_model_test tests/model_test.cpp launcher/src/json.cpp
                   launcher/src/model.cpp launcher/src/net.cpp)
    target_include_directories(amalgam_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_model_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(amalgam_model_test PRIVATE wininet advapi32)
    add_test(NAME amalgam_model_test COMMAND amalgam_model_test)

    add_executable(amalgam_profile_handoff_test
        tests/profile_handoff_test.cpp
        launcher/src/profile_handoff.cpp
        launcher/src/instances.cpp
        launcher/src/model.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_profile_handoff_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_profile_handoff_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(amalgam_profile_handoff_test PRIVATE wininet advapi32)
    add_test(NAME amalgam_profile_handoff_test COMMAND amalgam_profile_handoff_test)

    add_executable(amalgam_official_launcher_guard_test
        tests/official_launcher_guard_test.cpp
        launcher/src/official_launcher_guard.cpp)
    target_include_directories(amalgam_official_launcher_guard_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_official_launcher_guard_test PRIVATE UNICODE _UNICODE)
    add_test(NAME amalgam_official_launcher_guard_test COMMAND amalgam_official_launcher_guard_test)

    add_executable(amalgam_net_test tests/net_test.cpp launcher/src/net.cpp)
    target_include_directories(amalgam_net_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src
                                                      ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(amalgam_net_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(amalgam_net_test PRIVATE wininet advapi32 ws2_32)
    add_test(NAME amalgam_net_test COMMAND amalgam_net_test)

    add_executable(amalgam_maven_artifact_test
        tests/maven_artifact_test.cpp
        launcher/src/maven_artifact.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_maven_artifact_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_maven_artifact_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(amalgam_maven_artifact_test PRIVATE wininet advapi32)
    add_test(NAME amalgam_maven_artifact_test COMMAND amalgam_maven_artifact_test)

    add_executable(amalgam_performance_test
        tests/performance_test.cpp
        launcher/src/performance.cpp
        launcher/src/performance_cache.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_performance_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_performance_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(amalgam_performance_test PRIVATE wininet advapi32)
    add_test(NAME amalgam_performance_test COMMAND amalgam_performance_test)

    add_executable(amalgam_content_import_test
        tests/content_import_test.cpp
        launcher/src/instances.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_content_import_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_content_import_test PRIVATE UNICODE _UNICODE)
    target_link_libraries(amalgam_content_import_test PRIVATE wininet advapi32)
    add_test(NAME amalgam_content_import_test COMMAND amalgam_content_import_test)

    add_executable(amalgam_bedrock_test
        tests/bedrock_test.cpp
        launcher/src/bedrock.cpp
        launcher/src/extract.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_bedrock_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_bedrock_test PRIVATE UNICODE _UNICODE AML_BEDROCK_TESTING)
    target_link_libraries(amalgam_bedrock_test PRIVATE wininet advapi32 shell32 shlwapi)
    add_test(NAME amalgam_bedrock_test COMMAND amalgam_bedrock_test)

    add_executable(amalgam_extract_test
        tests/extract_test.cpp
        launcher/src/extract.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_extract_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_extract_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(amalgam_extract_test PRIVATE wininet advapi32)
    add_test(NAME amalgam_extract_test COMMAND amalgam_extract_test)

    add_executable(amalgam_natives_test
        tests/natives_test.cpp
        launcher/src/natives.cpp
        launcher/src/extract.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_natives_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_natives_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(amalgam_natives_test PRIVATE wininet advapi32)
    add_test(NAME amalgam_natives_test COMMAND amalgam_natives_test)

    add_executable(amalgam_auth_test
        tests/auth_test.cpp
        launcher/src/auth.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_auth_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_auth_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(amalgam_auth_test PRIVATE wininet advapi32 crypt32 shell32)
    add_test(NAME amalgam_auth_test COMMAND amalgam_auth_test)

    add_executable(amalgam_java_test
        tests/java_test.cpp
        launcher/src/java.cpp
        launcher/src/extract.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_java_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_java_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(amalgam_java_test PRIVATE wininet advapi32 shell32 shlwapi)
    add_test(NAME amalgam_java_test COMMAND amalgam_java_test)

    add_executable(amalgam_ui_model_test tests/ui_model_test.cpp)
    target_include_directories(amalgam_ui_model_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    add_test(NAME amalgam_ui_model_test COMMAND amalgam_ui_model_test)

    add_executable(amalgam_ui_async_request_test tests/ui_async_request_test.cpp)
    target_include_directories(amalgam_ui_async_request_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    add_test(NAME amalgam_ui_async_request_test COMMAND amalgam_ui_async_request_test)

    add_executable(amalgam_essentials_test tests/essentials_test.cpp)
    target_include_directories(amalgam_essentials_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    add_test(NAME amalgam_essentials_test COMMAND amalgam_essentials_test)

    add_executable(amalgam_essentials_lifecycle_test
        tests/essentials_lifecycle_test.cpp
        launcher/src/essentials_manager.cpp
        launcher/src/essentials_session.cpp
        launcher/src/essentials_address.cpp
        launcher/src/supabase.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_essentials_lifecycle_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_essentials_lifecycle_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(amalgam_essentials_lifecycle_test PRIVATE wininet advapi32 ws2_32)
    add_test(NAME amalgam_essentials_lifecycle_test COMMAND amalgam_essentials_lifecycle_test)

    add_executable(amalgam_server_run_state_test tests/server_run_state_test.cpp)
    target_include_directories(amalgam_server_run_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    add_test(NAME amalgam_server_run_state_test COMMAND amalgam_server_run_state_test)

    add_executable(amalgam_server_providers_test
        tests/server_providers_test.cpp
        launcher/src/server_providers.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_server_providers_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_server_providers_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(amalgam_server_providers_test PRIVATE wininet advapi32)
    add_test(NAME amalgam_server_providers_test COMMAND amalgam_server_providers_test)

    add_executable(amalgam_server_provision_test
        tests/server_provision_test.cpp
        launcher/src/server_provision.cpp
        launcher/src/server_providers.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp
        launcher/src/extract.cpp)
    target_include_directories(amalgam_server_provision_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_server_provision_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(amalgam_server_provision_test PRIVATE wininet advapi32 shell32)
    add_test(NAME amalgam_server_provision_test COMMAND amalgam_server_provision_test)

    add_executable(amalgam_cloud_hosting_test
        tests/cloud_hosting_test.cpp
        launcher/src/cloud_provider.cpp
        launcher/src/entitlements.cpp
        launcher/src/online_config.cpp
        launcher/src/supabase.cpp
        launcher/src/json.cpp
        launcher/src/net.cpp)
    target_include_directories(amalgam_cloud_hosting_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    target_compile_definitions(amalgam_cloud_hosting_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(amalgam_cloud_hosting_test PRIVATE wininet advapi32 shell32 ole32 ws2_32)
    add_test(NAME amalgam_cloud_hosting_test COMMAND amalgam_cloud_hosting_test)

    add_executable(amalgam_readiness_test tests/readiness_test.cpp)
    target_include_directories(amalgam_readiness_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    add_test(NAME amalgam_readiness_test COMMAND amalgam_readiness_test)

    add_executable(amalgam_version_catalog_test tests/version_catalog_test.cpp)
    target_include_directories(amalgam_version_catalog_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/launcher/src)
    add_test(NAME amalgam_version_catalog_test COMMAND amalgam_version_catalog_test)

    if(AMALGAM_BUILD_LAUNCHER)
        add_test(NAME amalgam_launcher_smoke COMMAND amalgam_launcher --check-java)
        set_tests_properties(amalgam_launcher_smoke PROPERTIES PASS_REGULAR_EXPRESSION "Java")
        add_test(NAME amalgam_prereq_smoke COMMAND amalgam_launcher --check-prereqs)
        set_tests_properties(amalgam_prereq_smoke PROPERTIES PASS_REGULAR_EXPRESSION "tar=OK")
        add_test(NAME amalgam_fixture_inventory_test
            COMMAND ${CMAKE_COMMAND}
                "-DAMALGAM_LAUNCHER=$<TARGET_FILE:amalgam_launcher>"
                "-DAMALGAM_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/../tools/launcher-visual-qa-cases.full.json"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_fixture_inventory_test.cmake)
        set_tests_properties(amalgam_fixture_inventory_test PROPERTIES TIMEOUT 20)
    endif()

    find_program(AMALGAM_JAVAC javac
        HINTS "$ENV{JAVA_HOME}/bin" "C:/Program Files/Android/openjdk/jdk-21.0.8/bin")
    find_program(AMALGAM_JAVA java
        HINTS "$ENV{JAVA_HOME}/bin" "C:/Program Files/Android/openjdk/jdk-21.0.8/bin")
    get_filename_component(AMALGAM_JAVA_TOOL_DIR "${AMALGAM_JAVAC}" DIRECTORY)
    find_program(AMALGAM_JAR jar HINTS "${AMALGAM_JAVA_TOOL_DIR}" NO_DEFAULT_PATH)
    if(AMALGAM_JAVAC AND AMALGAM_JAVA)
        add_test(NAME amalgam_java_bridge_test
            COMMAND ${CMAKE_COMMAND} -E env "AMALGAM_JAVAC=${AMALGAM_JAVAC}"
                    "AMALGAM_JAVA=${AMALGAM_JAVA}"
                    ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_java_bridge_test.cmake)
        add_test(NAME amalgam_neoforge_java_bridge_test
            COMMAND ${CMAKE_COMMAND} -E env "AMALGAM_JAVAC=${AMALGAM_JAVAC}"
                    "AMALGAM_JAVA=${AMALGAM_JAVA}"
                    ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_java_neoforge_bridge_test.cmake)
        add_test(NAME amalgam_forge_java_bridge_test
            COMMAND ${CMAKE_COMMAND} -E env "AMALGAM_JAVAC=${AMALGAM_JAVAC}"
                    "AMALGAM_JAVA=${AMALGAM_JAVA}"
                    ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_java_forge_bridge_test.cmake)
        if(AMALGAM_BUILD_LAUNCHER AND AMALGAM_JAR)
            add_test(NAME amalgam_server_transport_test
                COMMAND ${CMAKE_COMMAND}
                    "-DAMALGAM_LAUNCHER=$<TARGET_FILE:amalgam_launcher>"
                    "-DAMALGAM_JAVAC=${AMALGAM_JAVAC}"
                    "-DAMALGAM_JAVA=${AMALGAM_JAVA}"
                    "-DAMALGAM_JAR=${AMALGAM_JAR}"
                    "-DAMALGAM_WORK=${CMAKE_CURRENT_BINARY_DIR}/server-transport-test"
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_server_transport_test.cmake)
            set_tests_properties(amalgam_server_transport_test PROPERTIES TIMEOUT 30)
        endif()
    endif()
    if(AMALGAM_BUILD_LAUNCHER)
        add_test(NAME amalgam_client_bridge_test
            COMMAND $<TARGET_FILE:amalgam_launcher> --client-bridge-probe
                    "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(amalgam_client_bridge_test PROPERTIES TIMEOUT 20)
    endif()

    # assert() is what most of these tests check with, and the Release flags
    # define NDEBUG, which compiles every one of them out: a suite that reports
    # green whatever the code does. Undefine it for the test executables only.
    get_property(amalgam_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(amalgam_target IN LISTS amalgam_all_targets)
        if(amalgam_target MATCHES "_test$")
            target_compile_options(${amalgam_target} PRIVATE /UNDEBUG)
        endif()
    endforeach()
endif()

# Install rules: copy launcher, DLL, and branding assets to CMAKE_INSTALL_PREFIX.
# Usage: cmake --install cpp/build --prefix <destination>
if(AMALGAM_BUILD_LAUNCHER)
    install(TARGETS amalgam_launcher DESTINATION bin)
    install(TARGETS amalgam DESTINATION bin)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../docs/branding/
            DESTINATION branding
            FILES_MATCHING PATTERN "*.png" PATTERN "*.svg"
                           PATTERN "*.json" PATTERN "*.md")
endif()
