foreach(required_file
        "${AEGISY_SOURCE_DIR}/installer.iss"
        "${AEGISY_SOURCE_DIR}/.github/workflows/windows-package.yml"
        "${AEGISY_SOURCE_DIR}/package-windows.bat"
        "${AEGISY_SOURCE_DIR}/cmake/windows/AegisyClient.manifest.in"
        "${AEGISY_SOURCE_DIR}/cmake/windows/AegisyClient.rc.in")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Windows packaging policy file is missing: ${required_file}")
    endif()
endforeach()

file(READ "${AEGISY_SOURCE_DIR}/installer.iss" installer)
if(NOT installer MATCHES "MinVersion=10\\.0\\.17763")
    message(FATAL_ERROR "Inno Setup script does not enforce Windows 10 1809 minimum")
endif()

file(READ "${AEGISY_SOURCE_DIR}/cmake/windows/AegisyClient.manifest.in" manifest)
foreach(required_fragment
        "requestedExecutionLevel level=\"asInvoker\""
        "longPathAware"
        ">true</longPathAware>")
    if(NOT manifest MATCHES "${required_fragment}")
        message(FATAL_ERROR "Windows application manifest is missing: ${required_fragment}")
    endif()
endforeach()

file(READ "${AEGISY_SOURCE_DIR}/cmake/windows/AegisyClient.rc.in" resource)
if(NOT resource MATCHES "RT_MANIFEST")
    message(FATAL_ERROR "Windows resource script does not embed the application manifest")
endif()

file(READ "${AEGISY_SOURCE_DIR}/.github/workflows/windows-package.yml" workflow)
foreach(required_qt_module
        qtdeclarative
        qtpositioning
        qtwebchannel
        qtwebengine
        qtwebsockets)
    if(NOT workflow MATCHES "modules:[^\n]*${required_qt_module}")
        message(FATAL_ERROR
            "Windows workflow does not install required Qt module: ${required_qt_module}")
    endif()
endforeach()

file(READ "${AEGISY_SOURCE_DIR}/CMakeLists.txt" cmake_source)
string(FIND "${cmake_source}" "option(AEGISY_REQUIRE_QT6" require_qt6_option)
if(require_qt6_option EQUAL -1)
    message(FATAL_ERROR "CMake does not expose the explicit Qt 6 release gate")
endif()
foreach(required_qt_component
        Core
        Widgets
        Network
        Sql
        WebSockets
        WebChannel
        WebEngineWidgets)
    if(NOT cmake_source MATCHES "${required_qt_component}")
        message(FATAL_ERROR
            "Qt 6 release gate does not require component: ${required_qt_component}")
    endif()
endforeach()
if(NOT cmake_source MATCHES "Qt6 REQUIRED COMPONENTS")
    message(FATAL_ERROR "Qt 6 release gate does not fail closed on missing components")
endif()

file(READ "${AEGISY_SOURCE_DIR}/package-windows.bat" package_script)
foreach(release_source workflow package_script)
    string(FIND "${${release_source}}" "AEGISY_REQUIRE_QT6=ON" require_qt6_enabled)
    if(require_qt6_enabled EQUAL -1)
        message(FATAL_ERROR
            "Windows release path does not enable the complete Qt 6 gate: ${release_source}")
    endif()
endforeach()
foreach(required_policy_test windows_packaging_policy qt6_release_policy)
    if(NOT workflow MATCHES "${required_policy_test}")
        message(FATAL_ERROR
            "Windows validation job does not run policy test: ${required_policy_test}")
    endif()
endforeach()

message(STATUS
    "Windows packaging policy includes OS, manifest, and complete Qt SDK gates")
