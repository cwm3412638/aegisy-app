cmake_minimum_required(VERSION 3.16)
project(AegisyClient VERSION 2.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

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
)

# 可执行文件
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})
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
    # GUI 应用程序，不显示控制台
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# macOS 特定
if(APPLE)
    find_library(SECURITY_FRAMEWORK Security)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${SECURITY_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
    )
    # 生成 .app bundle，macdeployqt 才能打包并出 dmg
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Aegisy Client"
        MACOSX_BUNDLE_GUI_IDENTIFIER "cc.aegisy.client"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST_COPYRIGHT "© Aegisy"
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
endif()
