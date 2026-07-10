set(AEGISY_WINSPARKLE_VERSION "0.9.3")
set(AEGISY_WINSPARKLE_ARCHIVE
    "${CMAKE_BINARY_DIR}/_deps/WinSparkle-${AEGISY_WINSPARKLE_VERSION}.zip")
set(AEGISY_WINSPARKLE_ROOT
    "${CMAKE_BINARY_DIR}/_deps/WinSparkle-${AEGISY_WINSPARKLE_VERSION}")

if(NOT EXISTS "${AEGISY_WINSPARKLE_ROOT}/include/winsparkle.h")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps")
    if(NOT EXISTS "${AEGISY_WINSPARKLE_ARCHIVE}")
        message(STATUS "Downloading WinSparkle ${AEGISY_WINSPARKLE_VERSION}")
        file(DOWNLOAD
            "https://github.com/vslavik/winsparkle/releases/download/v${AEGISY_WINSPARKLE_VERSION}/WinSparkle-${AEGISY_WINSPARKLE_VERSION}.zip"
            "${AEGISY_WINSPARKLE_ARCHIVE}"
            EXPECTED_HASH
                "SHA256=745985f41d2ab26b2d5a1cf87d76e4ed851039db19038e50610eb25ea0b73772"
            TLS_VERIFY ON
            SHOW_PROGRESS
        )
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xvf "${AEGISY_WINSPARKLE_ARCHIVE}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/_deps"
        RESULT_VARIABLE AEGISY_WINSPARKLE_EXTRACT_RESULT
    )
    if(NOT AEGISY_WINSPARKLE_EXTRACT_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to extract WinSparkle")
    endif()
endif()

if(CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64"
        OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
    set(AEGISY_WINSPARKLE_BINARY_DIR
        "${AEGISY_WINSPARKLE_ROOT}/ARM64/Release")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(AEGISY_WINSPARKLE_BINARY_DIR
        "${AEGISY_WINSPARKLE_ROOT}/x64/Release")
else()
    set(AEGISY_WINSPARKLE_BINARY_DIR
        "${AEGISY_WINSPARKLE_ROOT}/Release")
endif()

add_library(AegisyWinSparkle SHARED IMPORTED GLOBAL)
set_target_properties(AegisyWinSparkle PROPERTIES
    IMPORTED_IMPLIB "${AEGISY_WINSPARKLE_BINARY_DIR}/WinSparkle.lib"
    IMPORTED_LOCATION "${AEGISY_WINSPARKLE_BINARY_DIR}/WinSparkle.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${AEGISY_WINSPARKLE_ROOT}/include"
)

function(aegisy_setup_winsparkle target)
    target_link_libraries(${target} PRIVATE AegisyWinSparkle)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${AEGISY_WINSPARKLE_BINARY_DIR}/WinSparkle.dll"
            "$<TARGET_FILE_DIR:${target}>/WinSparkle.dll"
        VERBATIM
    )
    install(FILES "${AEGISY_WINSPARKLE_BINARY_DIR}/WinSparkle.dll"
        DESTINATION bin)
endfunction()
