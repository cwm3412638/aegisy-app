cmake_minimum_required(VERSION 3.16)
project(AegisyClient VERSION 2.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(APPLE)
    enable_language(OBJCXX)
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING
            "Minimum supported macOS version" FORCE)
    endif()
endif()

if(WIN32)
    string(REGEX REPLACE "[^ -~]" "" AEGISY_ASCII_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT AEGISY_ASCII_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(AEGISY_NONASCII_CHECKOUT ON)
    endif()
    enable_language(RC)
endif()

# Release packaging requires the complete Qt 6 workbench SDK. Developer builds keep
# the Qt 5 and native-editor fallback unless the release gate is explicitly enabled.
option(AEGISY_REQUIRE_QT6 "Require the complete Qt 6 workbench SDK" OFF)
set(AEGISY_QT_BASE_COMPONENTS Core Widgets Network Sql WebSockets)
set(AEGISY_QT_WORKBENCH_COMPONENTS WebChannel WebEngineWidgets QuickWidgets)
if(AEGISY_REQUIRE_QT6)
    find_package(Qt6 REQUIRED COMPONENTS
        ${AEGISY_QT_BASE_COMPONENTS}
        ${AEGISY_QT_WORKBENCH_COMPONENTS})
else()
    find_package(Qt6 COMPONENTS ${AEGISY_QT_BASE_COMPONENTS} QUIET)
endif()

if(Qt6_FOUND)
    set(QT_VERSION_MAJOR 6)
    if(NOT AEGISY_REQUIRE_QT6)
        find_package(Qt6 QUIET COMPONENTS ${AEGISY_QT_WORKBENCH_COMPONENTS})
    endif()
else()
    message(STATUS "Qt6 not found, trying Qt5...")
    find_package(Qt5 5.14 REQUIRED COMPONENTS ${AEGISY_QT_BASE_COMPONENTS})
    set(QT_VERSION_MAJOR 5)
endif()

# 查找 OpenSSL
find_package(OpenSSL REQUIRED)

# Isolated experiments (optional)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/experiments/webengine/CMakeLists.txt")
    add_subdirectory(experiments/webengine)
endif()

set(AEGISY_UPDATE_PUBLIC_KEY
    "9DkeQEQcqz6Trrcmr1XRlZHDQHyeOj3DaVA2rtS3WA0="
    CACHE STRING "Ed25519 update and emergency-policy public key")
set(AEGISY_UPDATE_ARTIFACT_ROOT_KEY_ID ""
    CACHE STRING "Pinned update artifact Key Ring root Key ID")
set(AEGISY_UPDATE_ARTIFACT_ROOT_PUBLIC_KEY ""
    CACHE STRING "Pinned update artifact Key Ring root Ed25519 public key")

set(AEGISY_AAP_TRANSPORT_GENERATED_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/agent-runtime/aap-schema/generated/cpp")
set(AEGISY_AAP_TRANSPORT_RUNTIME_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/agent-runtime/aap-schema/runtime/cpp")
add_library(AegisyAapTransport STATIC
    ${AEGISY_AAP_TRANSPORT_GENERATED_DIR}/aap_transport_types_generated.cpp
    ${AEGISY_AAP_TRANSPORT_GENERATED_DIR}/aap_transport_types_generated.h
    ${AEGISY_AAP_TRANSPORT_RUNTIME_DIR}/aap_transport_runtime.cpp
    ${AEGISY_AAP_TRANSPORT_RUNTIME_DIR}/aap_transport_runtime.h
)
target_include_directories(AegisyAapTransport PUBLIC
    ${AEGISY_AAP_TRANSPORT_GENERATED_DIR}
    ${AEGISY_AAP_TRANSPORT_RUNTIME_DIR}
)
if(QT_VERSION_MAJOR EQUAL 6)
    target_link_libraries(AegisyAapTransport PUBLIC Qt6::Core)
else()
    target_link_libraries(AegisyAapTransport PUBLIC Qt5::Core)
endif()
if(MSVC)
    target_compile_options(AegisyAapTransport PRIVATE /W4 /WX)
else()
    target_compile_options(AegisyAapTransport PRIVATE -Wall -Wextra -Werror)
endif()

# 源文件
set(SOURCES
    src/main.cpp
    src/main_window.cpp
    src/login_dialog.cpp
    src/api_client.cpp
    src/companion_config_projection.cpp
    src/companion_key_management_projection.cpp
    src/companion_credential_broker.cpp
    src/companion_model_projection.cpp
    src/companion_usage_projection.cpp
    src/configuration_backup_store.cpp
    src/tool_manager.cpp
    src/process_command.cpp
    src/secure_storage.cpp
    src/api_keys_dialog.cpp
    src/account_dialog.cpp
    src/models_dialog.cpp
    src/image_generation_dialog.cpp
    src/chat_dialog.cpp
    src/skill_manager.cpp
    src/skills_dialog.cpp
    src/system_doctor_dialog.cpp
    src/usage_dialog.cpp
    src/gateway_manager.cpp
    src/gateway_dialog.cpp
    src/desktop_enhancement_manager.cpp
    src/desktop_enhancement_dialog.cpp
    src/profile_manager.cpp
    src/connect_wizard.cpp
    src/app_theme.cpp
    src/status_badge.cpp
    src/profile_archive.cpp
    src/runtime_status_store.cpp
    src/runtime_status_bar.cpp
    src/desktop_downloader.cpp
    src/mcp_config_dialog.cpp
    src/help_dialog.cpp
    src/agent_runtime_client.cpp
    src/artifact_manifest.cpp
    src/update_signing_key_ring.cpp
    src/update_signing_key_ring_cache.cpp
    src/update_artifact_set.cpp
    src/workbench_emergency_policy.cpp
    src/agent_workbench_widget.cpp
    src/feature_flags.cpp
    src/agent_workbench_window.cpp
    src/timeline_api.cpp
    resources.qrc
)

if(APPLE)
    list(APPEND SOURCES src/update_manager_mac.mm)
elseif(WIN32)
    list(APPEND SOURCES src/update_manager_win.cpp)
else()
    list(APPEND SOURCES src/update_manager_stub.cpp)
endif()

set(HEADERS
    include/main_window.h
    include/login_dialog.h
    include/api_client.h
    include/companion_config_projection.h
    include/companion_key_management_projection.h
    include/companion_credential_broker.h
    include/companion_model_projection.h
    include/companion_usage_projection.h
    include/configuration_backup_store.h
    include/credential_metadata.h
    include/tool_manager.h
    include/process_command.h
    include/secure_storage.h
    include/api_keys_dialog.h
    include/account_dialog.h
    include/models_dialog.h
    include/image_generation_dialog.h
    include/chat_dialog.h
    include/skill_manager.h
    include/skills_dialog.h
    include/system_doctor_dialog.h
    include/usage_dialog.h
    include/gateway_manager.h
    include/gateway_dialog.h
    include/desktop_enhancement_manager.h
    include/desktop_enhancement_dialog.h
    include/profile_manager.h
    include/connect_wizard.h
    include/app_theme.h
    include/status_badge.h
    include/profile_archive.h
    include/update_manager.h
    include/runtime_status_store.h
    include/runtime_status_bar.h
    include/desktop_downloader.h
    include/mcp_config_dialog.h
    include/help_dialog.h
    include/agent_runtime_client.h
    include/artifact_manifest.h
    include/canonical_path_policy.h
    include/update_signing_key_ring.h
    include/update_signing_key_ring_cache.h
    include/update_artifact_set.h
    include/workbench_emergency_policy.h
    include/agent_workbench_widget.h
    include/feature_flags.h
    include/agent_workbench_window.h
    include/timeline_api.h
)

# 可执行文件
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/aegisy-version.txt"
    CONTENT "${PROJECT_VERSION}\n")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    AEGISY_APP_VERSION="${PROJECT_VERSION}"
    AEGISY_UPDATE_PUBLIC_KEY="${AEGISY_UPDATE_PUBLIC_KEY}"
    AEGISY_UPDATE_ARTIFACT_ROOT_KEY_ID="${AEGISY_UPDATE_ARTIFACT_ROOT_KEY_ID}"
    AEGISY_UPDATE_ARTIFACT_ROOT_PUBLIC_KEY="${AEGISY_UPDATE_ARTIFACT_ROOT_PUBLIC_KEY}"
)

# Build the trusted local Monaco bundle only when the complete host toolchain is
# available. Other developer and target environments keep the Qt editor fallback.
find_program(NPM_EXECUTABLE npm)
find_program(NODE_EXECUTABLE node)
# Visual Studio custom build steps run every COMMAND line inside one
# generated .bat; invoking the npm batch shim there transfers control away
# and silently skips all later commands (the bundle and stamp are never
# produced). Invoke npm through node + npm-cli.js on Windows so every
# custom command line is a real executable that returns normally.
if(WIN32 AND NODE_EXECUTABLE)
    get_filename_component(AEGISY_NODE_ROOT "${NODE_EXECUTABLE}" DIRECTORY)
    set(AEGISY_NPM_CLI "${AEGISY_NODE_ROOT}/node_modules/npm/bin/npm-cli.js")
    if(EXISTS "${AEGISY_NPM_CLI}")
        set(AEGISY_NPM_COMMAND "${NODE_EXECUTABLE}" "${AEGISY_NPM_CLI}")
    endif()
endif()
if(NOT AEGISY_NPM_COMMAND AND NPM_EXECUTABLE)
    set(AEGISY_NPM_COMMAND "${NPM_EXECUTABLE}")
endif()
set(AEGISY_MONACO_ENABLED FALSE)
if(QT_VERSION_MAJOR EQUAL 6
        AND TARGET Qt6::WebChannel
        AND TARGET Qt6::WebEngineWidgets
        AND TARGET Qt6::QuickWidgets
        AND AEGISY_NPM_COMMAND)
    set(AEGISY_MONACO_ENABLED TRUE)
    set(AEGISY_WORKBENCH_WEB_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/workbench-web")
    set(AEGISY_WORKBENCH_WEB_BUILD_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/workbench-web")
    set(AEGISY_WORKBENCH_WEB_STAMP
        "${CMAKE_CURRENT_BINARY_DIR}/workbench-web-built.stamp")
    add_custom_command(
        OUTPUT "${AEGISY_WORKBENCH_WEB_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${AEGISY_WORKBENCH_WEB_BUILD_DIR}"
        COMMAND ${AEGISY_NPM_COMMAND} ci --no-audit --no-fund
        COMMAND ${CMAKE_COMMAND} -E env
            "AEGISY_WEB_OUT_DIR=${AEGISY_WORKBENCH_WEB_BUILD_DIR}"
            ${AEGISY_NPM_COMMAND} run build
        COMMAND ${CMAKE_COMMAND} -E touch "${AEGISY_WORKBENCH_WEB_STAMP}"
        DEPENDS
            "${AEGISY_WORKBENCH_WEB_SOURCE_DIR}/package.json"
            "${AEGISY_WORKBENCH_WEB_SOURCE_DIR}/package-lock.json"
            "${AEGISY_WORKBENCH_WEB_SOURCE_DIR}/build.mjs"
            "${AEGISY_WORKBENCH_WEB_SOURCE_DIR}/src/index.html"
            "${AEGISY_WORKBENCH_WEB_SOURCE_DIR}/src/editor.js"
            "${AEGISY_WORKBENCH_WEB_SOURCE_DIR}/src/style.css"
            "${AEGISY_WORKBENCH_WEB_SOURCE_DIR}/src/terminal.html"
            "${AEGISY_WORKBENCH_WEB_SOURCE_DIR}/src/terminal.js"
            "${AEGISY_WORKBENCH_WEB_SOURCE_DIR}/src/terminal.css"
        WORKING_DIRECTORY "${AEGISY_WORKBENCH_WEB_SOURCE_DIR}"
        COMMENT "Building trusted local Monaco workbench bundle"
        VERBATIM
    )
    if(APPLE)
        set(AEGISY_WORKBENCH_WEB_BUNDLE_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.app/Contents/Resources/workbench")
    elseif(CMAKE_CONFIGURATION_TYPES)
        set(AEGISY_WORKBENCH_WEB_BUNDLE_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/workbench")
    else()
        set(AEGISY_WORKBENCH_WEB_BUNDLE_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/workbench")
    endif()
    add_custom_target(aegisy_workbench_web ALL
        COMMAND ${CMAKE_COMMAND} -E remove_directory
            "${AEGISY_WORKBENCH_WEB_BUNDLE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${AEGISY_WORKBENCH_WEB_BUNDLE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${AEGISY_WORKBENCH_WEB_BUILD_DIR}"
            "${AEGISY_WORKBENCH_WEB_BUNDLE_DIR}"
        DEPENDS "${AEGISY_WORKBENCH_WEB_STAMP}"
        COMMENT "Bundling trusted local Monaco workbench"
        VERBATIM
    )
    add_dependencies(${PROJECT_NAME} aegisy_workbench_web)
    target_sources(${PROJECT_NAME} PRIVATE
        src/monaco_editor_bridge.cpp
        include/monaco_editor_bridge.h
        src/terminal_web_bridge.cpp
        include/terminal_web_bridge.h
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE AEGISY_HAS_MONACO=1)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        Qt6::WebChannel
        Qt6::WebEngineWidgets
    )
else()
    message(STATUS "Monaco workbench disabled: Qt WebEngine/WebChannel/QuickWidgets or npm unavailable")
endif()

# The Agent Workbench remains usable when Rust is unavailable, but a complete
# developer build also compiles and places the local AAP runtime beside the app.
find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin")
set(AEGISY_AGENT_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/agent-runtime")
# Keep the sidecar profile aligned with the desktop configuration.  Multi-
# configuration generators resolve the profile at build time so `--config
# Release` cannot accidentally package Cargo's debug binary.
if(CMAKE_CONFIGURATION_TYPES)
    set(AEGISY_AGENTD_PROFILE_DIR
        "$<IF:$<CONFIG:Release>,release,debug>")
    set(AEGISY_AGENTD_CARGO_RELEASE_ARG
        "$<$<CONFIG:Release>:--release>")
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(AEGISY_AGENTD_PROFILE_DIR "release")
    set(AEGISY_AGENTD_CARGO_RELEASE_ARG "--release")
else()
    set(AEGISY_AGENTD_PROFILE_DIR "debug")
    set(AEGISY_AGENTD_CARGO_RELEASE_ARG)
endif()
set(AEGISY_AGENTD_BINARY
    "${AEGISY_AGENT_RUNTIME_DIR}/target/${AEGISY_AGENTD_PROFILE_DIR}/aegisy-agentd${CMAKE_EXECUTABLE_SUFFIX}")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    AEGISY_AGENTD_DEV_PATH="${AEGISY_AGENTD_BINARY}"
)
if(CARGO_EXECUTABLE)
    if(APPLE)
        set(AEGISY_AGENTD_BUNDLE_DIR
            "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.app/Contents/MacOS")
    elseif(CMAKE_CONFIGURATION_TYPES)
        set(AEGISY_AGENTD_BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
    else()
        set(AEGISY_AGENTD_BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    add_custom_target(aegisy_agentd ALL
        COMMAND "${CARGO_EXECUTABLE}" build -p aegisy-agentd
            ${AEGISY_AGENTD_CARGO_RELEASE_ARG}
        WORKING_DIRECTORY "${AEGISY_AGENT_RUNTIME_DIR}"
        COMMENT "Building aegisy-agentd"
        VERBATIM
    )
    add_custom_target(aegisy_agentd_bundle ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${AEGISY_AGENTD_BUNDLE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${AEGISY_AGENTD_BINARY}"
            "${AEGISY_AGENTD_BUNDLE_DIR}"
        COMMENT "Bundling aegisy-agentd"
        VERBATIM
    )
    add_dependencies(aegisy_agentd_bundle aegisy_agentd)
    add_dependencies(${PROJECT_NAME} aegisy_agentd_bundle)
else()
    message(WARNING "cargo not found: Agent Workbench runtime will be unavailable")
endif()

# 包含目录
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${OPENSSL_INCLUDE_DIR}
)

# 链接库
if(QT_VERSION_MAJOR EQUAL 6)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        Qt6::Core
        Qt6::Widgets
        Qt6::Network
        Qt6::Sql
        Qt6::WebSockets
        OpenSSL::SSL
        OpenSSL::Crypto
    )
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        Qt5::Core
        Qt5::Widgets
        Qt5::Network
        Qt5::Sql
        Qt5::WebSockets
        OpenSSL::SSL
        OpenSSL::Crypto
    )
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE AegisyAapTransport)

# Windows 特定
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE crypt32)
    add_executable(AegisyTlsProbe tests/windows_tls_probe.cpp)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyTlsProbe PRIVATE Qt6::Core Qt6::Network)
    else()
        target_link_libraries(AegisyTlsProbe PRIVATE Qt5::Core Qt5::Network)
    endif()
    set(AEGISY_WINDOWS_UPDATE_FEED_URL
        "https://aegisy.cc/desktop/windows/appcast.xml"
        CACHE STRING "WinSparkle appcast URL")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        AEGISY_WINDOWS_UPDATE_FEED_URL="${AEGISY_WINDOWS_UPDATE_FEED_URL}"
        AEGISY_UPDATE_PUBLIC_KEY="${AEGISY_UPDATE_PUBLIC_KEY}"
    )
    include(cmake/WinSparkle.cmake)
    aegisy_setup_winsparkle(${PROJECT_NAME})

    set(AEGISY_WINDOWS_ICON_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/AegisyClient.ico")
    set(AEGISY_WINDOWS_MANIFEST_PATH
        "${CMAKE_CURRENT_BINARY_DIR}/AegisyClient.manifest")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/AegisyClient.manifest.in"
        "${AEGISY_WINDOWS_MANIFEST_PATH}"
        @ONLY
    )
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/AegisyClient.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/AegisyClient.rc"
        @ONLY
    )
    if(AEGISY_NONASCII_CHECKOUT)
        # The Visual Studio generator always compiles .rc sources with
        # rc.exe, which only accepts paths in the system ANSI codepage. A
        # non-ASCII checkout therefore compiles the resource with LLVM's
        # Unicode-aware llvm-rc and links the produced .res instead; the
        # linker itself handles Unicode paths.
        find_program(AEGISY_LLVM_RC NAMES llvm-rc
            HINTS "C:/Program Files/LLVM/bin")
        if(NOT AEGISY_LLVM_RC)
            message(FATAL_ERROR
                "llvm-rc (LLVM) is required for non-ASCII Windows checkouts")
        endif()
        set(AEGISY_RC_INCLUDES)
        foreach(AEGISY_RC_INCLUDE_SUBDIR um ucrt shared winrt)
            set(AEGISY_RC_INCLUDE_DIR
                "${CMAKE_WINDOWS_KITS_10_DIR}Include/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/${AEGISY_RC_INCLUDE_SUBDIR}")
            if(EXISTS "${AEGISY_RC_INCLUDE_DIR}")
                list(APPEND AEGISY_RC_INCLUDES "/i${AEGISY_RC_INCLUDE_DIR}")
            endif()
        endforeach()
        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/AegisyClient.res"
            COMMAND "${AEGISY_LLVM_RC}" /nologo ${AEGISY_RC_INCLUDES}
                "/fo${CMAKE_CURRENT_BINARY_DIR}/AegisyClient.res"
                "${CMAKE_CURRENT_BINARY_DIR}/AegisyClient.rc"
            DEPENDS
                "${CMAKE_CURRENT_BINARY_DIR}/AegisyClient.rc"
                "${AEGISY_WINDOWS_MANIFEST_PATH}"
                "${AEGISY_WINDOWS_ICON_PATH}"
            VERBATIM)
        target_sources(${PROJECT_NAME} PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/AegisyClient.res")
    else()
        target_sources(${PROJECT_NAME} PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/AegisyClient.rc")
    endif()
    # GUI 应用程序，不显示控制台
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# macOS 特定
if(APPLE)
    set(AEGISY_UPDATE_FEED_URL
        "https://aegisy.cc/desktop/macos/appcast.xml"
        CACHE STRING "Sparkle appcast URL")
    set(AEGISY_SPARKLE_PUBLIC_KEY
        "${AEGISY_UPDATE_PUBLIC_KEY}"
        CACHE STRING "Sparkle EdDSA public key")

    find_library(SECURITY_FRAMEWORK Security)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    set(MACOSX_ICON_FILE AegisyClient.icns)
    set(MACOSX_ICON_PATH ${CMAKE_CURRENT_SOURCE_DIR}/assets/${MACOSX_ICON_FILE})
    set_source_files_properties(${MACOSX_ICON_PATH} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(${PROJECT_NAME} PRIVATE ${MACOSX_ICON_PATH})
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${SECURITY_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
    )
    include(cmake/Sparkle.cmake)
    aegisy_setup_sparkle(${PROJECT_NAME})
    # 生成 .app bundle，macdeployqt 才能打包并出 dmg
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Aegisy Client"
        MACOSX_BUNDLE_GUI_IDENTIFIER "cc.aegisy.client"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "${MACOSX_ICON_FILE}"
        MACOSX_BUNDLE_INFO_PLIST_COPYRIGHT "© Aegisy"
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/Info.plist.in"
    )
endif()

# 安装规则
install(TARGETS ${PROJECT_NAME}
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

include(CTest)
if(BUILD_TESTING)
    if(NOT NODE_EXECUTABLE)
        message(FATAL_ERROR
            "BUILD_TESTING requires Node.js for the aap_generated_types gate")
    endif()
    if(NOT CARGO_EXECUTABLE)
        message(FATAL_ERROR
            "BUILD_TESTING requires Cargo for the aap_generated_types gate")
    endif()
    if(NODE_EXECUTABLE)
        add_test(NAME local_gateway_stream
            COMMAND "${NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/local_gateway_stream_test.js")
    endif()
    if(CARGO_EXECUTABLE)
        add_test(NAME agent_runtime_protocol
            COMMAND "${CARGO_EXECUTABLE}" test --workspace)
        set_tests_properties(agent_runtime_protocol PROPERTIES
            WORKING_DIRECTORY "${AEGISY_AGENT_RUNTIME_DIR}")
    endif()
    add_executable(AegisyAapGeneratedTypesTest
        tests/aap_generated_types_test.cpp
        agent-runtime/aap-schema/generated/cpp/aap_core_types_generated.cpp
        agent-runtime/aap-schema/generated/cpp/aap_core_types_generated.h
    )
    target_include_directories(AegisyAapGeneratedTypesTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/agent-runtime/aap-schema/generated/cpp)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyAapGeneratedTypesTest PRIVATE Qt6::Core)
    else()
        target_link_libraries(AegisyAapGeneratedTypesTest PRIVATE Qt5::Core)
    endif()
    add_test(NAME aap_generated_types
        COMMAND ${CMAKE_COMMAND}
            -DAEGISY_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DAEGISY_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DNODE_EXECUTABLE=${NODE_EXECUTABLE}
            -DCARGO_EXECUTABLE=${CARGO_EXECUTABLE}
            -DCPP_EXECUTABLE=$<TARGET_FILE:AegisyAapGeneratedTypesTest>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_aap_generated_types.cmake)
    add_executable(AegisyAapTransportRuntimeTest
        agent-runtime/aap-schema/runtime/cpp/aap_transport_runtime_test.cpp
    )
    add_executable(AegisyAapTransportGeneratedTypesTest
        tests/aap_transport_generated_types_test.cpp
    )
    foreach(transport_target IN ITEMS
            AegisyAapTransportRuntimeTest
            AegisyAapTransportGeneratedTypesTest)
        target_link_libraries(${transport_target} PRIVATE AegisyAapTransport)
        if(QT_VERSION_MAJOR EQUAL 6)
            target_link_libraries(${transport_target} PRIVATE Qt6::Core)
        else()
            target_link_libraries(${transport_target} PRIVATE Qt5::Core)
        endif()
        if(MSVC)
            target_compile_options(${transport_target} PRIVATE /W4 /WX)
        else()
            target_compile_options(${transport_target} PRIVATE -Wall -Wextra -Werror)
        endif()
    endforeach()
    set(AEGISY_AAP_TRANSPORT_TEST_SCHEMA_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/aap-传输-验证")
    file(MAKE_DIRECTORY "${AEGISY_AAP_TRANSPORT_TEST_SCHEMA_DIR}")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/agent-runtime/aap-schema/stable/v0.1/aap.schema.json"
        "${AEGISY_AAP_TRANSPORT_TEST_SCHEMA_DIR}/aap.schema.json"
        COPYONLY)
    add_test(NAME aap_transport_runtime
        COMMAND AegisyAapTransportRuntimeTest
            "${AEGISY_AAP_TRANSPORT_TEST_SCHEMA_DIR}/aap.schema.json")
    add_test(NAME aap_transport_generated_types
        COMMAND ${CMAKE_COMMAND}
            -DAEGISY_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DAEGISY_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DNODE_EXECUTABLE=${NODE_EXECUTABLE}
            -DCARGO_EXECUTABLE=${CARGO_EXECUTABLE}
            -DCPP_EXECUTABLE=$<TARGET_FILE:AegisyAapTransportGeneratedTypesTest>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/verify_aap_transport_generated_types.cmake)
    add_executable(AegisyProfileArchiveTest
        tests/profile_archive_test.cpp
        src/profile_archive.cpp
        include/profile_archive.h
    )
    target_include_directories(AegisyProfileArchiveTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${OPENSSL_INCLUDE_DIR}
    )
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyProfileArchiveTest PRIVATE
            Qt6::Core OpenSSL::Crypto)
    else()
        target_link_libraries(AegisyProfileArchiveTest PRIVATE
            Qt5::Core OpenSSL::Crypto)
    endif()
    add_test(NAME profile_archive_round_trip COMMAND AegisyProfileArchiveTest)

    add_executable(AegisyConfigurationBackupStoreTest
        tests/configuration_backup_store_test.cpp
        src/configuration_backup_store.cpp
        include/configuration_backup_store.h
    )
    target_include_directories(AegisyConfigurationBackupStoreTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${OPENSSL_INCLUDE_DIR}
    )
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyConfigurationBackupStoreTest PRIVATE
            Qt6::Core OpenSSL::Crypto)
    else()
        target_link_libraries(AegisyConfigurationBackupStoreTest PRIVATE
            Qt5::Core OpenSSL::Crypto)
    endif()
    if(MSVC)
        target_compile_options(AegisyConfigurationBackupStoreTest PRIVATE /W4 /WX)
    else()
        target_compile_options(AegisyConfigurationBackupStoreTest PRIVATE
            -Wall -Wextra -Werror)
    endif()
    add_test(NAME configuration_backup_store
        COMMAND AegisyConfigurationBackupStoreTest)

    add_executable(AegisyCompanionConfigurationCacheTest
        tests/companion_configuration_cache_test.cpp
        src/companion_configuration_cache.cpp
        src/companion_config_projection.cpp
        src/companion_model_projection.cpp
        include/companion_configuration_cache.h
        include/companion_config_projection.h
        include/companion_model_projection.h
    )
    target_include_directories(AegisyCompanionConfigurationCacheTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${OPENSSL_INCLUDE_DIR}
    )
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyCompanionConfigurationCacheTest PRIVATE
            Qt6::Core OpenSSL::Crypto)
    else()
        target_link_libraries(AegisyCompanionConfigurationCacheTest PRIVATE
            Qt5::Core OpenSSL::Crypto)
    endif()
    if(MSVC)
        target_compile_options(AegisyCompanionConfigurationCacheTest PRIVATE
            /W4 /WX)
    else()
        target_compile_options(AegisyCompanionConfigurationCacheTest PRIVATE
            -Wall -Wextra -Werror)
    endif()
    add_test(NAME companion_configuration_cache
        COMMAND AegisyCompanionConfigurationCacheTest)

    add_executable(AegisyProfileActivationTest
        tests/profile_activation_test.cpp
        src/profile_manager.cpp
        src/profile_archive.cpp
        src/secure_storage.cpp
        include/profile_manager.h
        include/profile_archive.h
        include/secure_storage.h
    )
    target_include_directories(AegisyProfileActivationTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${OPENSSL_INCLUDE_DIR}
    )
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyProfileActivationTest PRIVATE
            Qt6::Core OpenSSL::Crypto)
    else()
        target_link_libraries(AegisyProfileActivationTest PRIVATE
            Qt5::Core OpenSSL::Crypto)
    endif()
    if(APPLE)
        target_link_libraries(AegisyProfileActivationTest PRIVATE
            ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    elseif(WIN32)
        target_link_libraries(AegisyProfileActivationTest PRIVATE crypt32)
    endif()
    add_test(NAME profile_activation_per_tool COMMAND AegisyProfileActivationTest)

    add_executable(AegisyToolManagerRuntimeTest
        tests/tool_manager_runtime_test.cpp
        tests/qt_test_failure_sink.h
        src/tool_manager.cpp
        src/configuration_backup_store.cpp
        src/secure_storage.cpp
        src/process_command.cpp
        include/tool_manager.h
        include/configuration_backup_store.h
        include/secure_storage.h
        include/process_command.h
    )
    target_include_directories(AegisyToolManagerRuntimeTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(AegisyToolManagerRuntimeTest PRIVATE
        AEGISY_TOOL_MANAGER_RUNTIME_TEST=1)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyToolManagerRuntimeTest PRIVATE
            Qt6::Core OpenSSL::Crypto)
    else()
        target_link_libraries(AegisyToolManagerRuntimeTest PRIVATE
            Qt5::Core OpenSSL::Crypto)
    endif()
    if(APPLE)
        target_link_libraries(AegisyToolManagerRuntimeTest PRIVATE
            ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    elseif(WIN32)
        target_link_libraries(AegisyToolManagerRuntimeTest PRIVATE crypt32)
    endif()
    add_test(NAME tool_manager_runtime_registry COMMAND AegisyToolManagerRuntimeTest)

    add_executable(AegisyToolManagerGatewayConfigTest
        tests/tool_manager_gateway_config_test.cpp
        src/tool_manager.cpp
        src/configuration_backup_store.cpp
        src/secure_storage.cpp
        src/process_command.cpp
        include/tool_manager.h
        include/configuration_backup_store.h
        include/secure_storage.h
        include/process_command.h
    )
    target_include_directories(AegisyToolManagerGatewayConfigTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyToolManagerGatewayConfigTest PRIVATE
            Qt6::Core OpenSSL::Crypto)
    else()
        target_link_libraries(AegisyToolManagerGatewayConfigTest PRIVATE
            Qt5::Core OpenSSL::Crypto)
    endif()
    if(APPLE)
        target_link_libraries(AegisyToolManagerGatewayConfigTest PRIVATE
            ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    elseif(WIN32)
        target_link_libraries(AegisyToolManagerGatewayConfigTest PRIVATE crypt32)
    endif()
    add_test(NAME tool_manager_gateway_config COMMAND AegisyToolManagerGatewayConfigTest)

    add_executable(AegisyProductScopePolicyTest
        tests/product_scope_policy_test.cpp
    )
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyProductScopePolicyTest PRIVATE Qt6::Core)
    else()
        target_link_libraries(AegisyProductScopePolicyTest PRIVATE Qt5::Core)
    endif()
    add_test(NAME product_scope_policy
        COMMAND AegisyProductScopePolicyTest ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(AegisySecureStorageFreshReadPolicyTest
        tests/secure_storage_fresh_read_policy_test.cpp
        include/secure_storage.h
    )
    target_include_directories(AegisySecureStorageFreshReadPolicyTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisySecureStorageFreshReadPolicyTest PRIVATE Qt6::Core)
    else()
        target_link_libraries(AegisySecureStorageFreshReadPolicyTest PRIVATE Qt5::Core)
    endif()
    if(MSVC)
        target_compile_options(AegisySecureStorageFreshReadPolicyTest PRIVATE /W4 /WX)
    else()
        target_compile_options(AegisySecureStorageFreshReadPolicyTest PRIVATE
            -Wall -Wextra -Werror)
    endif()
    add_test(NAME secure_storage_fresh_read_policy
        COMMAND AegisySecureStorageFreshReadPolicyTest ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(AegisyCompanionConfigProjectionTest
        tests/companion_config_projection_test.cpp
        src/companion_config_projection.cpp
        include/companion_config_projection.h
    )
    target_include_directories(AegisyCompanionConfigProjectionTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyCompanionConfigProjectionTest PRIVATE Qt6::Core)
    else()
        target_link_libraries(AegisyCompanionConfigProjectionTest PRIVATE Qt5::Core)
    endif()
    add_test(NAME companion_config_projection
        COMMAND AegisyCompanionConfigProjectionTest)

    add_executable(AegisyCompanionCredentialBrokerTest
        tests/companion_credential_broker_test.cpp
        src/companion_config_projection.cpp
        src/companion_credential_broker.cpp
        src/secure_storage.cpp
        include/companion_config_projection.h
        include/companion_credential_broker.h
        include/secure_storage.h
    )
    target_include_directories(AegisyCompanionCredentialBrokerTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyCompanionCredentialBrokerTest PRIVATE Qt6::Core)
    else()
        target_link_libraries(AegisyCompanionCredentialBrokerTest PRIVATE Qt5::Core)
    endif()
    if(APPLE)
        target_link_libraries(AegisyCompanionCredentialBrokerTest PRIVATE
            ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    elseif(WIN32)
        target_link_libraries(AegisyCompanionCredentialBrokerTest PRIVATE crypt32)
    endif()
    add_test(NAME companion_credential_broker
        COMMAND AegisyCompanionCredentialBrokerTest)

    add_executable(AegisyCompanionModelProjectionTest
        tests/companion_model_projection_test.cpp
        src/companion_model_projection.cpp
        include/companion_model_projection.h
    )
    target_include_directories(AegisyCompanionModelProjectionTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyCompanionModelProjectionTest PRIVATE Qt6::Core)
    else()
        target_link_libraries(AegisyCompanionModelProjectionTest PRIVATE Qt5::Core)
    endif()
    add_test(NAME companion_model_projection
        COMMAND AegisyCompanionModelProjectionTest)

    add_executable(AegisyCompanionUsageProjectionTest
        tests/companion_usage_projection_test.cpp
        src/companion_config_projection.cpp
        src/companion_usage_projection.cpp
        include/companion_config_projection.h
        include/companion_usage_projection.h
    )
    target_include_directories(AegisyCompanionUsageProjectionTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyCompanionUsageProjectionTest PRIVATE Qt6::Core)
    else()
        target_link_libraries(AegisyCompanionUsageProjectionTest PRIVATE Qt5::Core)
    endif()
    add_test(NAME companion_usage_projection
        COMMAND AegisyCompanionUsageProjectionTest)

    add_executable(AegisyCompanionKeyManagementProjectionTest
        tests/companion_key_management_projection_test.cpp
        src/companion_config_projection.cpp
        src/companion_key_management_projection.cpp
        include/companion_config_projection.h
        include/companion_key_management_projection.h
    )
    target_include_directories(AegisyCompanionKeyManagementProjectionTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyCompanionKeyManagementProjectionTest PRIVATE Qt6::Core)
    else()
        target_link_libraries(AegisyCompanionKeyManagementProjectionTest PRIVATE Qt5::Core)
    endif()
    add_test(NAME companion_key_management_projection
        COMMAND AegisyCompanionKeyManagementProjectionTest)

    find_program(AEGISY_NODE_EXECUTABLE NAMES node node.exe)
    if(AEGISY_NODE_EXECUTABLE)
        add_test(NAME local_gateway_security
            COMMAND ${AEGISY_NODE_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/local_gateway_test.js)
    endif()

    add_executable(AegisyDesktopEnhancementTest
        tests/desktop_enhancement_test.cpp
        src/desktop_enhancement_manager.cpp
        src/process_command.cpp
        include/desktop_enhancement_manager.h
        include/process_command.h
    )
    target_include_directories(AegisyDesktopEnhancementTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyDesktopEnhancementTest PRIVATE
            Qt6::Core Qt6::Network Qt6::Sql Qt6::WebSockets)
    else()
        target_link_libraries(AegisyDesktopEnhancementTest PRIVATE
            Qt5::Core Qt5::Network Qt5::Sql Qt5::WebSockets)
    endif()
    add_test(NAME desktop_enhancement_history_sync
        COMMAND AegisyDesktopEnhancementTest)

    add_executable(AegisyApiClientAccountTest
        tests/api_client_account_test.cpp
        src/api_client.cpp
        src/companion_config_projection.cpp
        src/companion_credential_broker.cpp
        src/companion_model_projection.cpp
        src/companion_usage_projection.cpp
        src/companion_key_management_projection.cpp
        src/secure_storage.cpp
        include/api_client.h
        include/companion_config_projection.h
        include/companion_credential_broker.h
        include/companion_model_projection.h
        include/companion_usage_projection.h
        include/companion_key_management_projection.h
        include/secure_storage.h
    )
    target_include_directories(AegisyApiClientAccountTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyApiClientAccountTest PRIVATE
            Qt6::Core Qt6::Network)
    else()
        target_link_libraries(AegisyApiClientAccountTest PRIVATE
            Qt5::Core Qt5::Network)
    endif()
    if(APPLE)
        target_link_libraries(AegisyApiClientAccountTest PRIVATE
            ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    elseif(WIN32)
        target_link_libraries(AegisyApiClientAccountTest PRIVATE crypt32)
    endif()
    add_test(NAME api_client_account_and_keys
        COMMAND AegisyApiClientAccountTest)

    add_executable(AegisyCompanionKeyManagementApiTest
        tests/companion_key_management_api_test.cpp
        src/api_client.cpp
        src/companion_config_projection.cpp
        src/companion_credential_broker.cpp
        src/companion_model_projection.cpp
        src/companion_usage_projection.cpp
        src/companion_key_management_projection.cpp
        src/secure_storage.cpp
        include/api_client.h
        include/companion_config_projection.h
        include/companion_credential_broker.h
        include/companion_model_projection.h
        include/companion_usage_projection.h
        include/companion_key_management_projection.h
        include/secure_storage.h
    )
    target_include_directories(AegisyCompanionKeyManagementApiTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(AegisyCompanionKeyManagementApiTest PRIVATE
        AEGISY_SECURE_STORAGE_REMOVE_TESTING=1)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyCompanionKeyManagementApiTest PRIVATE
            Qt6::Core Qt6::Network)
    else()
        target_link_libraries(AegisyCompanionKeyManagementApiTest PRIVATE
            Qt5::Core Qt5::Network)
    endif()
    if(APPLE)
        target_link_libraries(AegisyCompanionKeyManagementApiTest PRIVATE
            ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    elseif(WIN32)
        target_link_libraries(AegisyCompanionKeyManagementApiTest PRIVATE crypt32)
    endif()
    add_test(NAME companion_key_management_api
        COMMAND AegisyCompanionKeyManagementApiTest)

    add_executable(AegisyApiKeysDialogProjectionTest
        tests/api_keys_dialog_projection_test.cpp
        src/api_keys_dialog.cpp
        src/api_client.cpp
        src/app_theme.cpp
        src/companion_config_projection.cpp
        src/companion_credential_broker.cpp
        src/companion_model_projection.cpp
        src/companion_usage_projection.cpp
        src/companion_key_management_projection.cpp
        src/secure_storage.cpp
        include/api_keys_dialog.h
        include/api_client.h
        include/app_theme.h
        include/companion_config_projection.h
        include/companion_credential_broker.h
        include/companion_model_projection.h
        include/companion_usage_projection.h
        include/companion_key_management_projection.h
        include/secure_storage.h
    )
    target_include_directories(AegisyApiKeysDialogProjectionTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyApiKeysDialogProjectionTest PRIVATE
            Qt6::Core Qt6::Network Qt6::Widgets)
    else()
        target_link_libraries(AegisyApiKeysDialogProjectionTest PRIVATE
            Qt5::Core Qt5::Network Qt5::Widgets)
    endif()
    if(APPLE)
        target_link_libraries(AegisyApiKeysDialogProjectionTest PRIVATE
            ${SECURITY_FRAMEWORK} ${COREFOUNDATION_FRAMEWORK})
    elseif(WIN32)
        target_link_libraries(AegisyApiKeysDialogProjectionTest PRIVATE crypt32)
    endif()
    add_test(NAME api_keys_dialog_projection
        COMMAND AegisyApiKeysDialogProjectionTest)
    set_tests_properties(api_keys_dialog_projection PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(AegisyWorkbenchEmergencyPolicyTest
        tests/workbench_emergency_policy_test.cpp
        src/workbench_emergency_policy.cpp
        include/workbench_emergency_policy.h
    )
    target_include_directories(AegisyWorkbenchEmergencyPolicyTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyWorkbenchEmergencyPolicyTest PRIVATE
            Qt6::Core OpenSSL::Crypto)
    else()
        target_link_libraries(AegisyWorkbenchEmergencyPolicyTest PRIVATE
            Qt5::Core OpenSSL::Crypto)
    endif()
    add_test(NAME workbench_emergency_policy
        COMMAND AegisyWorkbenchEmergencyPolicyTest)

    add_executable(AegisyDesktopDownloaderTest
        tests/desktop_downloader_test.cpp
        src/desktop_downloader.cpp
        src/app_theme.cpp
        include/desktop_downloader.h
        include/app_theme.h
    )
    target_include_directories(AegisyDesktopDownloaderTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyDesktopDownloaderTest PRIVATE
            Qt6::Core Qt6::Widgets Qt6::Network)
    else()
        target_link_libraries(AegisyDesktopDownloaderTest PRIVATE
            Qt5::Core Qt5::Widgets Qt5::Network)
    endif()
    add_test(NAME desktop_download_proxy_contract
        COMMAND AegisyDesktopDownloaderTest)

    add_executable(AegisySkillManagerTest
        tests/skill_manager_test.cpp
        src/skill_manager.cpp
        include/skill_manager.h
        resources.qrc
    )
    target_include_directories(AegisySkillManagerTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisySkillManagerTest PRIVATE Qt6::Core Qt6::Network)
    else()
        target_link_libraries(AegisySkillManagerTest PRIVATE Qt5::Core Qt5::Network)
    endif()
    add_test(NAME skill_manager_install_and_routing COMMAND AegisySkillManagerTest)

    add_executable(AegisyRuntimeStatusStoreTest
        tests/runtime_status_store_test.cpp
        src/runtime_status_store.cpp
        include/runtime_status_store.h
    )
    target_include_directories(AegisyRuntimeStatusStoreTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyRuntimeStatusStoreTest PRIVATE Qt6::Core)
    else()
        target_link_libraries(AegisyRuntimeStatusStoreTest PRIVATE Qt5::Core)
    endif()
    add_test(NAME runtime_status_store COMMAND AegisyRuntimeStatusStoreTest)

    add_executable(AegisyRuntimeStatusBarRenderTest
        tests/runtime_status_bar_render_test.cpp
        src/runtime_status_bar.cpp
        src/runtime_status_store.cpp
        include/runtime_status_bar.h
        include/runtime_status_store.h
    )
    target_include_directories(AegisyRuntimeStatusBarRenderTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyRuntimeStatusBarRenderTest PRIVATE
            Qt6::Core Qt6::Widgets)
    else()
        target_link_libraries(AegisyRuntimeStatusBarRenderTest PRIVATE
            Qt5::Core Qt5::Widgets)
    endif()
    add_test(NAME runtime_status_bar_render COMMAND AegisyRuntimeStatusBarRenderTest)
    set_tests_properties(runtime_status_bar_render PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(AegisyAgentWorkbenchRenderTest
        tests/agent_workbench_render_test.cpp
        src/agent_workbench_widget.cpp
        src/agent_runtime_client.cpp
        src/artifact_manifest.cpp
        src/app_theme.cpp
        include/agent_workbench_widget.h
        include/agent_runtime_client.h
        include/artifact_manifest.h
        include/app_theme.h
        resources.qrc
    )
    target_compile_definitions(AegisyAgentWorkbenchRenderTest PRIVATE
        AEGISY_APP_VERSION="${PROJECT_VERSION}"
        AEGISY_AGENTD_DEV_PATH="${AEGISY_AGENTD_BINARY}"
    )
    if(CARGO_EXECUTABLE)
        target_compile_definitions(AegisyAgentWorkbenchRenderTest PRIVATE
            AEGISY_EXPECT_AGENTD=1)
        add_dependencies(AegisyAgentWorkbenchRenderTest aegisy_agentd)
    endif()
    target_include_directories(AegisyAgentWorkbenchRenderTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyAgentWorkbenchRenderTest PRIVATE
            Qt6::Core Qt6::Widgets Qt6::Network AegisyAapTransport)
    else()
        target_link_libraries(AegisyAgentWorkbenchRenderTest PRIVATE
            Qt5::Core Qt5::Widgets Qt5::Network AegisyAapTransport)
    endif()
    add_test(NAME agent_workbench_render COMMAND AegisyAgentWorkbenchRenderTest)
    set_tests_properties(agent_workbench_render PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;AEGISY_AGENT_BACKEND=preview")
    add_test(NAME agent_workbench_failure_channel
        COMMAND "${CMAKE_COMMAND}"
                "-DAEGISY_TEST_EXECUTABLE=$<TARGET_FILE:AegisyAgentWorkbenchRenderTest>"
                "-DAEGISY_TEST_LABEL=agent_workbench_render"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/qt_test_failure_channel_test.cmake")

    add_executable(AegisyAgentRuntimeEnvironmentTest
        tests/agent_runtime_environment_test.cpp
        src/agent_runtime_client.cpp
        src/artifact_manifest.cpp
        include/agent_runtime_client.h
        include/artifact_manifest.h
    )
    target_compile_definitions(AegisyAgentRuntimeEnvironmentTest PRIVATE
        AEGISY_APP_VERSION="${PROJECT_VERSION}"
        AEGISY_AGENTD_DEV_PATH="${AEGISY_AGENTD_BINARY}"
    )
    target_include_directories(AegisyAgentRuntimeEnvironmentTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyAgentRuntimeEnvironmentTest PRIVATE
            Qt6::Core Qt6::Network AegisyAapTransport)
    else()
        target_link_libraries(AegisyAgentRuntimeEnvironmentTest PRIVATE
            Qt5::Core Qt5::Network AegisyAapTransport)
    endif()
    add_test(NAME agent_runtime_environment
        COMMAND AegisyAgentRuntimeEnvironmentTest)

    if(WIN32 AND CARGO_EXECUTABLE)
        add_executable(AegisyAgentRuntimeWindowsNamedPipeTest
            tests/agent_runtime_windows_named_pipe_compile_test.cpp
            src/agent_runtime_client.cpp
            src/artifact_manifest.cpp
            include/agent_runtime_client.h
            include/artifact_manifest.h
        )
        target_compile_definitions(AegisyAgentRuntimeWindowsNamedPipeTest PRIVATE
            AEGISY_APP_VERSION="${PROJECT_VERSION}"
            AEGISY_AGENTD_DEV_PATH="${AEGISY_AGENTD_BINARY}"
        )
        target_include_directories(AegisyAgentRuntimeWindowsNamedPipeTest PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
        if(QT_VERSION_MAJOR EQUAL 6)
            target_link_libraries(AegisyAgentRuntimeWindowsNamedPipeTest PRIVATE
                Qt6::Core Qt6::Network AegisyAapTransport)
        else()
            target_link_libraries(AegisyAgentRuntimeWindowsNamedPipeTest PRIVATE
                Qt5::Core Qt5::Network AegisyAapTransport)
        endif()
        target_link_libraries(AegisyAgentRuntimeWindowsNamedPipeTest PRIVATE Advapi32)
        add_dependencies(AegisyAgentRuntimeWindowsNamedPipeTest aegisy_agentd)
        add_test(NAME agent_runtime_windows_named_pipe
            COMMAND AegisyAgentRuntimeWindowsNamedPipeTest)
        set_tests_properties(agent_runtime_windows_named_pipe PROPERTIES
            TIMEOUT 180)
    endif()

    if(APPLE AND CARGO_EXECUTABLE)
        add_executable(AegisyAgentRuntimeMacosSocketTest
            tests/agent_runtime_macos_socket_test.cpp
            src/agent_runtime_client.cpp
            src/artifact_manifest.cpp
            include/agent_runtime_client.h
            include/artifact_manifest.h
        )
        target_compile_definitions(AegisyAgentRuntimeMacosSocketTest PRIVATE
            AEGISY_APP_VERSION="${PROJECT_VERSION}"
            AEGISY_AGENTD_DEV_PATH="${AEGISY_AGENTD_BINARY}"
            AEGISY_EXPECT_AGENTD=1
        )
        target_include_directories(AegisyAgentRuntimeMacosSocketTest PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
        if(QT_VERSION_MAJOR EQUAL 6)
            target_link_libraries(AegisyAgentRuntimeMacosSocketTest PRIVATE
                Qt6::Core Qt6::Network AegisyAapTransport)
        else()
            target_link_libraries(AegisyAgentRuntimeMacosSocketTest PRIVATE
                Qt5::Core Qt5::Network AegisyAapTransport)
        endif()
        add_dependencies(AegisyAgentRuntimeMacosSocketTest aegisy_agentd)
        add_test(NAME agent_runtime_macos_socket
            COMMAND AegisyAgentRuntimeMacosSocketTest)
        set_tests_properties(agent_runtime_macos_socket PROPERTIES
            ENVIRONMENT "AEGISY_AGENT_BACKEND=preview")
    endif()

    add_executable(AegisyArtifactManifestTest
        tests/artifact_manifest_test.cpp
        src/artifact_manifest.cpp
        include/artifact_manifest.h
        include/canonical_path_policy.h
    )
    target_include_directories(AegisyArtifactManifestTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyArtifactManifestTest PRIVATE
            Qt6::Core AegisyAapTransport)
    else()
        target_link_libraries(AegisyArtifactManifestTest PRIVATE
            Qt5::Core AegisyAapTransport)
    endif()
    add_test(NAME artifact_manifest_verification
        COMMAND AegisyArtifactManifestTest)
    if(CARGO_EXECUTABLE)
        add_dependencies(AegisyArtifactManifestTest aegisy_agentd)
        add_test(NAME artifact_manifest_runtime_startup
            COMMAND AegisyArtifactManifestTest
                    --runtime-startup
                    "${AEGISY_AGENTD_BINARY}"
                    "${CMAKE_COMMAND}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_artifact_manifest.cmake")
        set_tests_properties(artifact_manifest_runtime_startup PROPERTIES TIMEOUT 120)
    endif()
    add_test(NAME artifact_manifest_generation
        COMMAND "${CMAKE_COMMAND}"
                -DAEGISY_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DAEGISY_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DMANIFEST_VERIFIER=$<TARGET_FILE:AegisyArtifactManifestTest>
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/artifact_manifest_generator_test.cmake")

    add_executable(AegisyUpdateSigningKeyRingTest
        tests/update_signing_key_ring_test.cpp
        src/update_signing_key_ring.cpp
        include/update_signing_key_ring.h
    )
    target_include_directories(AegisyUpdateSigningKeyRingTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(AegisyUpdateSigningKeyRingTest PRIVATE
        AEGISY_UPDATE_SIGNING_KEY_RING_TESTING=1)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyUpdateSigningKeyRingTest PRIVATE
            Qt6::Core OpenSSL::Crypto AegisyAapTransport)
    else()
        target_link_libraries(AegisyUpdateSigningKeyRingTest PRIVATE
            Qt5::Core OpenSSL::Crypto AegisyAapTransport)
    endif()
    if(MSVC)
        target_compile_options(AegisyUpdateSigningKeyRingTest PRIVATE /W4 /WX)
    else()
        target_compile_options(AegisyUpdateSigningKeyRingTest PRIVATE
            -Wall -Wextra -Werror)
    endif()
    add_test(NAME update_signing_key_ring_contract
        COMMAND AegisyUpdateSigningKeyRingTest)

    add_executable(AegisyUpdateSigningKeyRingCacheTest
        tests/update_signing_key_ring_cache_test.cpp
        src/update_signing_key_ring_cache.cpp
        include/update_signing_key_ring_cache.h
        src/update_signing_key_ring.cpp
        include/update_signing_key_ring.h
    )
    target_include_directories(AegisyUpdateSigningKeyRingCacheTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(AegisyUpdateSigningKeyRingCacheTest PRIVATE
        AEGISY_UPDATE_SIGNING_KEY_RING_TESTING=1)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyUpdateSigningKeyRingCacheTest PRIVATE
            Qt6::Core OpenSSL::Crypto AegisyAapTransport)
    else()
        target_link_libraries(AegisyUpdateSigningKeyRingCacheTest PRIVATE
            Qt5::Core OpenSSL::Crypto AegisyAapTransport)
    endif()
    if(MSVC)
        target_compile_options(AegisyUpdateSigningKeyRingCacheTest PRIVATE
            /W4 /WX)
    else()
        target_compile_options(AegisyUpdateSigningKeyRingCacheTest PRIVATE
            -Wall -Wextra -Werror)
    endif()
    add_test(NAME update_signing_key_ring_cache_integrity
        COMMAND AegisyUpdateSigningKeyRingCacheTest)

    add_executable(AegisyUpdateArtifactSetProductionFixture
        tests/update_artifact_set_production_fixture.cpp
        src/update_signing_key_ring.cpp
        include/update_signing_key_ring.h
        src/update_artifact_set.cpp
        include/update_artifact_set.h
        src/artifact_manifest.cpp
        include/artifact_manifest.h
    )
    target_include_directories(AegisyUpdateArtifactSetProductionFixture PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(AegisyUpdateArtifactSetProductionFixture PRIVATE
        AEGISY_UPDATE_ARTIFACT_ROOT_KEY_ID="artifact-fixture-root"
        AEGISY_UPDATE_ARTIFACT_ROOT_PUBLIC_KEY="LJuaQtV63/1Q5usdpUPek62X022XbSorlzXnFC8/uFk=")
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyUpdateArtifactSetProductionFixture PRIVATE
            Qt6::Core OpenSSL::Crypto AegisyAapTransport)
    else()
        target_link_libraries(AegisyUpdateArtifactSetProductionFixture PRIVATE
            Qt5::Core OpenSSL::Crypto AegisyAapTransport)
    endif()
    if(MSVC)
        target_compile_options(AegisyUpdateArtifactSetProductionFixture PRIVATE
            /W4 /WX)
    else()
        target_compile_options(AegisyUpdateArtifactSetProductionFixture PRIVATE
            -Wall -Wextra -Werror)
    endif()

    add_executable(AegisyUpdateArtifactSetTest
        tests/update_artifact_set_test.cpp
        src/update_signing_key_ring.cpp
        include/update_signing_key_ring.h
        src/update_artifact_set.cpp
        include/update_artifact_set.h
        src/artifact_manifest.cpp
        include/artifact_manifest.h
    )
    target_include_directories(AegisyUpdateArtifactSetTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(AegisyUpdateArtifactSetTest PRIVATE
        AEGISY_UPDATE_ARTIFACT_SET_TESTING=1
        AEGISY_UPDATE_SIGNING_KEY_RING_TESTING=1)
    add_dependencies(AegisyUpdateArtifactSetTest
        AegisyUpdateArtifactSetProductionFixture)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyUpdateArtifactSetTest PRIVATE
            Qt6::Core OpenSSL::Crypto AegisyAapTransport)
    else()
        target_link_libraries(AegisyUpdateArtifactSetTest PRIVATE
            Qt5::Core OpenSSL::Crypto AegisyAapTransport)
    endif()
    if(MSVC)
        target_compile_options(AegisyUpdateArtifactSetTest PRIVATE /W4 /WX)
    else()
        target_compile_options(AegisyUpdateArtifactSetTest PRIVATE
            -Wall -Wextra -Werror)
    endif()
    add_test(NAME update_artifact_set_compatibility
        COMMAND AegisyUpdateArtifactSetTest
                $<TARGET_FILE:AegisyUpdateArtifactSetProductionFixture>)

    add_executable(AegisyUpdateProgressRecordTest
        tests/update_progress_record_test.cpp
        src/update_progress_record.cpp
        include/update_progress_record.h
    )
    target_include_directories(AegisyUpdateProgressRecordTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(QT_VERSION_MAJOR EQUAL 6)
        target_link_libraries(AegisyUpdateProgressRecordTest PRIVATE
            Qt6::Core AegisyAapTransport)
    else()
        target_link_libraries(AegisyUpdateProgressRecordTest PRIVATE
            Qt5::Core AegisyAapTransport)
    endif()
    if(MSVC)
        target_compile_options(AegisyUpdateProgressRecordTest PRIVATE /W4 /WX)
    else()
        target_compile_options(AegisyUpdateProgressRecordTest PRIVATE
            -Wall -Wextra -Werror)
    endif()
    add_test(NAME update_progress_record_integrity
        COMMAND AegisyUpdateProgressRecordTest)

    add_test(NAME macos_ci_policy
        COMMAND "${CMAKE_COMMAND}"
                -DAEGISY_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/macos_ci_policy_test.cmake")
    add_test(NAME windows_packaging_policy
        COMMAND "${CMAKE_COMMAND}"
                -DAEGISY_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DAEGISY_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_packaging_policy_test.cmake")
    add_test(NAME windows_debug_pipe_policy
        COMMAND "${CMAKE_COMMAND}"
                -DAEGISY_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_debug_pipe_policy_test.cmake")
    if(QT_VERSION_MAJOR EQUAL 6)
        add_test(NAME qt6_release_policy
            COMMAND "${CMAKE_COMMAND}"
                    -DAEGISY_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                    -DAEGISY_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                    -DAEGISY_QT6_DIR=${Qt6_DIR}
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/qt6_release_policy_test.cmake")
    endif()

    if(AEGISY_MONACO_ENABLED)
        add_executable(AegisyMonacoEditorRenderTest
            tests/monaco_editor_render_test.cpp
            src/agent_workbench_widget.cpp
            src/agent_runtime_client.cpp
            src/artifact_manifest.cpp
            src/monaco_editor_bridge.cpp
            src/terminal_web_bridge.cpp
            src/app_theme.cpp
            include/agent_workbench_widget.h
            include/agent_runtime_client.h
            include/artifact_manifest.h
            include/monaco_editor_bridge.h
            include/terminal_web_bridge.h
            include/app_theme.h
            resources.qrc
        )
        target_compile_definitions(AegisyMonacoEditorRenderTest PRIVATE
            AEGISY_APP_VERSION="${PROJECT_VERSION}"
            AEGISY_AGENTD_DEV_PATH="${AEGISY_AGENTD_BINARY}"
            AEGISY_EXPECT_AGENTD=1
            AEGISY_HAS_MONACO=1
        )
        target_include_directories(AegisyMonacoEditorRenderTest PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include)
        target_link_libraries(AegisyMonacoEditorRenderTest PRIVATE
            Qt6::Core
            Qt6::Widgets
            Qt6::Network
            Qt6::QuickWidgets
            Qt6::WebChannel
            Qt6::WebEngineWidgets
            AegisyAapTransport
        )
        add_dependencies(AegisyMonacoEditorRenderTest
            aegisy_agentd
            aegisy_workbench_web)
        add_test(NAME monaco_editor_render COMMAND AegisyMonacoEditorRenderTest)
        set(monaco_editor_render_environment
            "AEGISY_AGENT_BACKEND=preview"
            "AEGISY_WORKBENCH_WEB_ROOT=${AEGISY_WORKBENCH_WEB_BUILD_DIR}")
        if(WIN32)
            # The GitHub Windows runner has no accelerated graphics context.
            # Keep Qt Quick and Chromium on their shared D3D11 path while
            # requesting Qt's software-adapter preference, then retain bounded
            # context diagnostics if the renderer still fails to initialize.
            list(APPEND monaco_editor_render_environment
                "QT_QPA_PLATFORM=windows"
                "QT_QUICK_BACKEND=rhi"
                "QSG_RHI_BACKEND=d3d11"
                "QSG_RHI_PREFER_SOFTWARE_RENDERER=1"
                "QT_FORCE_STDERR_LOGGING=1"
                "QTWEBENGINE_DISABLE_SANDBOX=1"
                "QTWEBENGINE_CHROMIUM_FLAGS=--no-sandbox --enable-logging=stderr"
                "QT_LOGGING_RULES=qt.webenginecontext.debug=true"
                "QSG_INFO=1")
        else()
            list(APPEND monaco_editor_render_environment
                "QT_QPA_PLATFORM=offscreen"
                "QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu")
        endif()
        set_tests_properties(monaco_editor_render PROPERTIES
            ENVIRONMENT "${monaco_editor_render_environment}")
        add_test(NAME monaco_editor_failure_channel
            COMMAND "${CMAKE_COMMAND}"
                    "-DAEGISY_TEST_EXECUTABLE=$<TARGET_FILE:AegisyMonacoEditorRenderTest>"
                    "-DAEGISY_TEST_LABEL=monaco_editor_render"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/qt_test_failure_channel_test.cmake")
    endif()
endif()

# Isolated Qt WebEngine experiments (tasks 2.1-2.4) - now in experiments/webengine/CMakeLists.txt
option(AEGISY_BUILD_WEBENGINE_EXPERIMENT "Build isolated Qt WebEngine experiment" OFF)
