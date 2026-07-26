foreach(required_file
        "${AEGISY_SOURCE_DIR}/installer.iss"
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

message(STATUS "Windows packaging policy includes MinVersion 10.0.17763 and longPathAware=true")
