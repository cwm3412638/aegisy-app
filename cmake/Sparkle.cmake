include_guard(GLOBAL)

function(aegisy_setup_sparkle target_name)
    if(NOT APPLE)
        return()
    endif()

    set(SPARKLE_VERSION "2.9.4" CACHE STRING "Pinned Sparkle framework version")
    set(SPARKLE_SHA256
        "ce89daf967db1e1893ed3ebd67575ed82d3902563e3191ca92aaec9164fbdef9")
    set(SPARKLE_URL
        "https://github.com/sparkle-project/Sparkle/releases/download/${SPARKLE_VERSION}/Sparkle-${SPARKLE_VERSION}.tar.xz")
    set(SPARKLE_DEPS_DIR "${CMAKE_BINARY_DIR}/_deps")
    set(SPARKLE_ARCHIVE "${SPARKLE_DEPS_DIR}/Sparkle-${SPARKLE_VERSION}.tar.xz")
    set(SPARKLE_ROOT "${SPARKLE_DEPS_DIR}/sparkle-${SPARKLE_VERSION}")

    file(MAKE_DIRECTORY "${SPARKLE_DEPS_DIR}")
    if(EXISTS "${SPARKLE_ARCHIVE}")
        file(SHA256 "${SPARKLE_ARCHIVE}" SPARKLE_ARCHIVE_SHA256)
        if(NOT SPARKLE_ARCHIVE_SHA256 STREQUAL SPARKLE_SHA256)
            file(REMOVE "${SPARKLE_ARCHIVE}")
        endif()
    endif()

    if(NOT EXISTS "${SPARKLE_ARCHIVE}")
        message(STATUS "Downloading Sparkle ${SPARKLE_VERSION}...")
        file(DOWNLOAD
            "${SPARKLE_URL}"
            "${SPARKLE_ARCHIVE}"
            EXPECTED_HASH "SHA256=${SPARKLE_SHA256}"
            TLS_VERIFY ON
            SHOW_PROGRESS
            STATUS SPARKLE_DOWNLOAD_STATUS
        )
        list(GET SPARKLE_DOWNLOAD_STATUS 0 SPARKLE_DOWNLOAD_CODE)
        if(NOT SPARKLE_DOWNLOAD_CODE EQUAL 0)
            list(GET SPARKLE_DOWNLOAD_STATUS 1 SPARKLE_DOWNLOAD_MESSAGE)
            message(FATAL_ERROR "Unable to download Sparkle: ${SPARKLE_DOWNLOAD_MESSAGE}")
        endif()
    endif()

    if(NOT EXISTS "${SPARKLE_ROOT}/Sparkle.framework")
        file(REMOVE_RECURSE "${SPARKLE_ROOT}")
        file(MAKE_DIRECTORY "${SPARKLE_ROOT}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E tar xJf "${SPARKLE_ARCHIVE}"
            WORKING_DIRECTORY "${SPARKLE_ROOT}"
            RESULT_VARIABLE SPARKLE_EXTRACT_RESULT
        )
        if(NOT SPARKLE_EXTRACT_RESULT EQUAL 0)
            message(FATAL_ERROR "Unable to extract Sparkle ${SPARKLE_VERSION}")
        endif()
    endif()

    unset(AEGISY_SPARKLE_FRAMEWORK CACHE)
    find_library(AEGISY_SPARKLE_FRAMEWORK
        NAMES Sparkle
        PATHS "${SPARKLE_ROOT}"
        NO_DEFAULT_PATH
    )
    if(NOT AEGISY_SPARKLE_FRAMEWORK)
        message(FATAL_ERROR "Sparkle.framework was not found in ${SPARKLE_ROOT}")
    endif()

    target_compile_options(${target_name} PRIVATE
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>"
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-F${SPARKLE_ROOT}>"
    )
    target_link_libraries(${target_name} PRIVATE "${AEGISY_SPARKLE_FRAMEWORK}")
    set_property(TARGET ${target_name} APPEND PROPERTY
        BUILD_RPATH "@executable_path/../Frameworks")
    set_property(TARGET ${target_name} APPEND PROPERTY
        INSTALL_RPATH "@executable_path/../Frameworks")
    set_property(TARGET ${target_name} PROPERTY BUILD_WITH_INSTALL_RPATH TRUE)

    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
                "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/Frameworks"
        COMMAND /usr/bin/ditto
                "${SPARKLE_ROOT}/Sparkle.framework"
                "$<TARGET_BUNDLE_DIR:${target_name}>/Contents/Frameworks/Sparkle.framework"
        COMMENT "Embedding Sparkle ${SPARKLE_VERSION}"
        VERBATIM
    )

    set(AEGISY_SPARKLE_ROOT "${SPARKLE_ROOT}" PARENT_SCOPE)
endfunction()
