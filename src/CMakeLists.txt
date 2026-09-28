cmake_minimum_required(VERSION 3.15)
project(ffgl-omt VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
set(FFGL_ROOT "" CACHE PATH "Root of the resolume/ffgl repository")
set(LIBOMT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/libomt"
    CACHE PATH "Directory containing bin/ include/ lib/ from the libomt release")

if(NOT FFGL_ROOT)
    message(FATAL_ERROR "Please set FFGL_ROOT to the root of your resolume/ffgl clone")
endif()

# ---------------------------------------------------------------------------
# Collect FFGL SDK source files
# We compile these DIRECTLY into each plugin DLL (not as a static lib)
# so that __declspec(dllexport) on plugMain flows through correctly.
# ---------------------------------------------------------------------------
file(GLOB FFGL_LIB_SOURCES
    "${FFGL_ROOT}/source/lib/ffgl/*.cpp"
    "${FFGL_ROOT}/source/lib/ffglex/*.cpp"
)

# ---------------------------------------------------------------------------
# libomt imported shared library
#
# The OMT plugins are not tracked in this repository (see .gitignore); they are
# built only when their sources happen to be present in the working tree.
# ---------------------------------------------------------------------------
set(OMT_SOURCES_PRESENT FALSE)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/source/plugins/OMTSend/OMTSend.cpp")
    set(OMT_SOURCES_PRESENT TRUE)
endif()

if(OMT_SOURCES_PRESENT)
    add_library(libomt SHARED IMPORTED)

    if(WIN32)
        set_target_properties(libomt PROPERTIES
            IMPORTED_LOCATION             "${LIBOMT_ROOT}/bin/libomt.dll"
            IMPORTED_IMPLIB               "${LIBOMT_ROOT}/lib/libomt.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBOMT_ROOT}/include"
        )
    elseif(APPLE)
        set_target_properties(libomt PROPERTIES
            IMPORTED_LOCATION             "${LIBOMT_ROOT}/bin/libomt.dylib"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBOMT_ROOT}/include"
        )
    else()
        set_target_properties(libomt PROPERTIES
            IMPORTED_LOCATION             "${LIBOMT_ROOT}/bin/libomt.so"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBOMT_ROOT}/include"
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# WebView2 SDK – optional, needed only for the ChromeBrowser plugin
# ---------------------------------------------------------------------------
set(WEBVIEW2_ROOT "" CACHE PATH "Root of the WebView2 SDK (must contain include/ lib/ bin/)")

if(NOT WEBVIEW2_ROOT)
    find_package(webview2 QUIET)
    if(webview2_FOUND)
        set(WEBVIEW2_AVAILABLE TRUE)
        message(STATUS "WebView2 SDK found via find_package")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/webview2/include/WebView2.h")
        set(WEBVIEW2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/webview2")
        set(WEBVIEW2_AVAILABLE TRUE)
        message(STATUS "WebView2 SDK found at ${WEBVIEW2_ROOT}")
    else()
        set(WEBVIEW2_AVAILABLE FALSE)
        message(WARNING "WebView2 SDK not found. ChromeBrowser plugin will NOT be built. "
                        "Install via vcpkg (vcpkg install webview2) or place SDK in deps/webview2/")
    endif()
endif()

# ---------------------------------------------------------------------------
# Helper macro – builds one FFGL plugin DLL
# ---------------------------------------------------------------------------
# Optional keyword:
#   NO_LIBOMT — skip linking libomt (for plugins that don't use OMT)
# ---------------------------------------------------------------------------
macro(ffgl_plugin TARGET_NAME)
    cmake_parse_arguments(ARG "NO_LIBOMT" "OUTPUT" "SOURCES" ${ARGN})

    add_library(${TARGET_NAME} SHARED
        ${ARG_SOURCES}
        ${FFGL_LIB_SOURCES}
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
        OUTPUT_NAME "${ARG_OUTPUT}"
        PREFIX      ""
    )

    if(WIN32)
        set_target_properties(${TARGET_NAME} PROPERTIES SUFFIX ".dll")
    elseif(APPLE)
        set_target_properties(${TARGET_NAME} PROPERTIES
            BUNDLE           TRUE
            BUNDLE_EXTENSION "bundle"
        )
    endif()

    target_include_directories(${TARGET_NAME} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/source/shared"
        "${FFGL_ROOT}/source/lib"
        "${FFGL_ROOT}/source/lib/ffglex"
        "${FFGL_ROOT}/deps/glew-2.1.0/include"
    )

    target_compile_definitions(${TARGET_NAME} PRIVATE GLEW_STATIC)

    if(NOT ARG_NO_LIBOMT)
        target_link_libraries(${TARGET_NAME} PRIVATE libomt)
    endif()

    if(WIN32)
        target_link_libraries(${TARGET_NAME} PRIVATE
            "${FFGL_ROOT}/deps/glew-2.1.0/lib/Release/x64/glew32s.lib"
            opengl32.lib
        )

        if(NOT ARG_NO_LIBOMT)
            add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${LIBOMT_ROOT}/bin/libomt.dll"
                    "${LIBOMT_ROOT}/bin/libvmx.dll"
                    $<TARGET_FILE_DIR:${TARGET_NAME}>
                COMMENT "Copying OMT runtime DLLs next to ${TARGET_NAME}"
            )
        endif()
    endif()
endmacro()

# ---------------------------------------------------------------------------
# OMT plugins - only when their sources are present locally
# ---------------------------------------------------------------------------
if(OMT_SOURCES_PRESENT)
    ffgl_plugin(OMTSend
        SOURCES
            source/plugins/OMTSend/OMTSend.cpp
            source/plugins/OMTSend/OMTSend.h
        OUTPUT OMTSend
    )

    ffgl_plugin(OMTReceive
        SOURCES
            source/plugins/OMTReceive/OMTReceive.cpp
            source/plugins/OMTReceive/OMTReceive.h
        OUTPUT OMTReceive
    )
endif()

ffgl_plugin(MinTest
    SOURCES
        source/plugins/MinTest/MinTest.cpp
        source/plugins/MinTest/MinTest.h
    OUTPUT MinTest
)

# ---------------------------------------------------------------------------
# ChromeBrowser – only built when WebView2 SDK is available
# ---------------------------------------------------------------------------
if(WEBVIEW2_AVAILABLE)
    ffgl_plugin(ChromeBrowser
        NO_LIBOMT
        SOURCES
            source/plugins/ChromeBrowser/ChromeBrowser_fixed.cpp
            source/plugins/ChromeBrowser/ChromeBrowser.h
        OUTPUT ChromeBrowser
    )

    if(WEBVIEW2_ROOT)
        target_include_directories(ChromeBrowser PRIVATE
            "${WEBVIEW2_ROOT}/include"
        )
        if(EXISTS "${WEBVIEW2_ROOT}/lib/WebView2Loader.dll.lib")
            target_link_libraries(ChromeBrowser PRIVATE
                "${WEBVIEW2_ROOT}/lib/WebView2Loader.dll.lib")
        elseif(EXISTS "${WEBVIEW2_ROOT}/lib/x64/WebView2Loader.dll.lib")
            target_link_libraries(ChromeBrowser PRIVATE
                "${WEBVIEW2_ROOT}/lib/x64/WebView2Loader.dll.lib")
        endif()

        # Copy WebView2Loader.dll to the output directory
        set(_wv2_dll "")
        if(EXISTS "${WEBVIEW2_ROOT}/bin/WebView2Loader.dll")
            set(_wv2_dll "${WEBVIEW2_ROOT}/bin/WebView2Loader.dll")
        elseif(EXISTS "${WEBVIEW2_ROOT}/bin/x64/WebView2Loader.dll")
            set(_wv2_dll "${WEBVIEW2_ROOT}/bin/x64/WebView2Loader.dll")
        endif()
        if(_wv2_dll)
            add_custom_command(TARGET ChromeBrowser POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_wv2_dll}"
                    $<TARGET_FILE_DIR:ChromeBrowser>
                COMMENT "Copied WebView2Loader.dll next to ChromeBrowser.dll"
            )
        endif()
        unset(_wv2_dll)
    else()
        # find_package version sets up includes/libs automatically
        target_link_libraries(ChromeBrowser PRIVATE WebView2::WebView2)
    endif()
endif()
