foreach(required_file
        "${AEGISY_SOURCE_DIR}/installer.iss"
        "${AEGISY_SOURCE_DIR}/.github/workflows/windows-package.yml"
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
if(NOT cmake_source MATCHES "DEFINED Qt6_DIR AND NOT Qt6_DIR STREQUAL")
    message(FATAL_ERROR "Explicit Qt6_DIR does not fail closed on missing Qt 6 components")
endif()
if(NOT cmake_source MATCHES "Qt6 REQUIRED COMPONENTS")
    message(FATAL_ERROR "Explicit Qt 6 SDK is not configured as a required release input")
endif()

message(STATUS
    "Windows packaging policy includes OS, manifest, and complete Qt SDK gates")
