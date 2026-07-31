cmake_minimum_required(VERSION 3.16)
project(AegisyWebEngineExperiment)

# This is an isolated experiment - does not affect production build
option(AEGISY_BUILD_WEBENGINE_EXPERIMENT "Build Qt WebEngine experiment" OFF)

if(NOT AEGISY_BUILD_WEBENGINE_EXPERIMENT)
    return()
endif()

# Find Qt with WebEngine
if(QT_VERSION_MAJOR EQUAL 6)
    find_package(Qt6 REQUIRED COMPONENTS Core Widgets WebEngineWidgets WebChannel)
    set(QT_LIBS Qt6::Core Qt6::Widgets Qt6::WebEngineWidgets Qt6::WebChannel)
else()
    find_package(Qt5 REQUIRED COMPONENTS Core Widgets WebEngineWidgets WebChannel)
    set(QT_LIBS Qt5::Core Qt5::Widgets Qt5::WebEngineWidgets Qt5::WebChannel)
endif()

# Create experiment executable
add_executable(aegisy_webengine_experiment
    main.cpp
)

target_link_libraries(aegisy_webengine_experiment PRIVATE ${QT_LIBS})

# Task 2.2: Secure local bundle with network blocking
add_executable(aegisy_webengine_secure_bundle
    secure_bundle.cpp
)

target_link_libraries(aegisy_webengine_secure_bundle PRIVATE ${QT_LIBS})

# Task 2.3: QWebChannel bridge with typed IDs, origin checks, size limits
add_executable(aegisy_webengine_webchannel_bridge
    webchannel_bridge.cpp
)

target_link_libraries(aegisy_webengine_webchannel_bridge PRIVATE ${QT_LIBS})

# Task 2.4: Monaco editor integration
add_executable(aegisy_webengine_monaco_editor
    monaco_editor.cpp
)

target_link_libraries(aegisy_webengine_monaco_editor PRIVATE ${QT_LIBS})

# Set output directory
set_target_properties(aegisy_webengine_experiment aegisy_webengine_secure_bundle
    aegisy_webengine_webchannel_bridge aegisy_webengine_monaco_editor PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

message(STATUS "Qt WebEngine experiment enabled - target: aegisy_webengine_experiment")
