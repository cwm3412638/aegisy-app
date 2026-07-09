cmake_minimum_required(VERSION 3.16)
project(AegisyClient VERSION 1.0.0 LANGUAGES CXX)

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
    src/config_manager.cpp
    src/env_detector.cpp
    src/secure_storage.cpp
    src/api_keys_dialog.cpp
    src/env_config_dialog.cpp
    src/env_manager_dialog.cpp
    src/models_dialog.cpp
    src/quick_setup_wizard.cpp
)

set(HEADERS
    include/main_window.h
    include/login_dialog.h
    include/api_client.h
    include/config_manager.h
    include/env_detector.h
    include/secure_storage.h
    include/api_keys_dialog.h
    include/env_config_dialog.h
    include/env_manager_dialog.h
    include/models_dialog.h
    include/quick_setup_wizard.h
)

# 可执行文件
add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

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
endif()

# 安装规则
install(TARGETS ${PROJECT_NAME}
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
