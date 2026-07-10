cmake_minimum_required(VERSION 3.16)
project(AegisyClient VERSION 2.3.1 LANGUAGES CXX)

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
    enable_language(RC)
endif()

# 查找 Qt6
find_package(Qt6 COMPONENTS Core Widgets Network QUIET)

if(NOT Qt6_FOUND)
    message(STATUS "Qt6 not found, trying Qt5...")
    find_package(Qt5 5.15 REQUIRED COMPONENTS Core Widgets Network)
    set(QT_VERSION_MAJOR 5)
else()
    set(QT_VERSION_MAJOR 6)
endif()

# 查找 OpenSSL
find_package(OpenSSL REQUIRED)

# 源文件
set(SOURCES
    src/main.cpp
    src/main_window.cpp
    src/login_dialog.cpp
    src/api_client.cpp
    src/tool_manager.cpp
    src/secure_storage.cpp
    src/api_keys_dialog.cpp
    src/models_dialog.cpp
    src/profile_manager.cpp
    src/connect_wizard.cpp
    src/app_theme.cpp
    src/profile_archive.cpp
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
    include/tool_manager.h
    include/secure_storage.h
    include/api_keys_dialog.h
    include/models_dialog.h
    include/profile_manager.h
    include/connect_wizard.h
    include/app_theme.h
    include/profile_archive.h
    include/update_manager.h
)

# 可执行文件
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/aegisy-version.txt"
    CONTENT "${PROJECT_VERSION}\n")
target_compile_definitions(${PROJECT_NAME} PRIVATE
    AEGISY_APP_VERSION="${PROJECT_VERSION}"
)

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
        OpenSSL::SSL
        OpenSSL::Crypto
    )
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        Qt5::Core
        Qt5::Widgets
        Qt5::Network
        OpenSSL::SSL
        OpenSSL::Crypto
    )
endif()

# Windows 特定
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE crypt32)
    set(AEGISY_WINDOWS_UPDATE_FEED_URL
        "https://aegisy.cc/desktop/windows/appcast.xml"
        CACHE STRING "WinSparkle appcast URL")
    set(AEGISY_UPDATE_PUBLIC_KEY
        "9DkeQEQcqz6Trrcmr1XRlZHDQHyeOj3DaVA2rtS3WA0="
        CACHE STRING "Ed25519 update public key")
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        AEGISY_WINDOWS_UPDATE_FEED_URL="${AEGISY_WINDOWS_UPDATE_FEED_URL}"
        AEGISY_UPDATE_PUBLIC_KEY="${AEGISY_UPDATE_PUBLIC_KEY}"
    )
    include(cmake/WinSparkle.cmake)
    aegisy_setup_winsparkle(${PROJECT_NAME})

    set(AEGISY_WINDOWS_ICON_PATH
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/AegisyClient.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/windows/AegisyClient.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/AegisyClient.rc"
        @ONLY
    )
    target_sources(${PROJECT_NAME} PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/AegisyClient.rc")
    # GUI 应用程序，不显示控制台
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# macOS 特定
if(APPLE)
    set(AEGISY_UPDATE_FEED_URL
        "https://aegisy.cc/desktop/macos/appcast.xml"
        CACHE STRING "Sparkle appcast URL")
    set(AEGISY_SPARKLE_PUBLIC_KEY
        "9DkeQEQcqz6Trrcmr1XRlZHDQHyeOj3DaVA2rtS3WA0="
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
endif()
