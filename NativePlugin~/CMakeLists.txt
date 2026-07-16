cmake_minimum_required(VERSION 3.20)
project(GfxPluginMDI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Diagnostic logging — verbose output from the backends (PSO hook decisions,
# bytecode reflection results, ExecuteMDI counts, etc.). Defaults to OFF for
# Release builds (DebugLog becomes a no-op the optimizer strips) and ON for
# Debug builds. Force it on with -DMDI_DEBUG_LOG=ON to debug release builds.
option(MDI_DEBUG_LOG "Enable native plugin debug logging" OFF)

set(SOURCES
    src/MultiDrawIndirect.cpp
    src/MDIBackend_Vulkan.cpp
    src/MDIBackend_GLES.cpp
)

set(HEADERS
    src/MDILog.h
    src/MDIBackend.h
    src/MDIBackend_Vulkan.h
    src/MDIBackend_GLES.h
    src/MDIBackend_Stub.h
    src/vulkan/vulkan.h
    src/gles/gles_minimal.h
    src/Unity/IUnityInterface.h
    src/Unity/IUnityGraphics.h
    src/Unity/IUnityGraphicsVulkan.h
    src/Unity/IUnityRenderingExtensions.h
)

if (WIN32)
    list(APPEND SOURCES
        src/MDIBackend_D3D12.cpp
        src/MDIBackend_D3D11.cpp
    )
    list(APPEND HEADERS
        src/MDIBackend_D3D12.h
        src/MDIBackend_D3D11.h
        src/Unity/IUnityGraphicsD3D11.h
        src/Unity/IUnityGraphicsD3D12.h
    )
endif()

if (APPLE)
    enable_language(OBJCXX)
    list(APPEND SOURCES src/MDIBackend_Metal.mm)
    list(APPEND HEADERS
        src/MDIBackend_Metal.h
        src/Unity/IUnityGraphicsMetal.h
    )
endif()

if (APPLE)
    # Unity loads native plugins on macOS as bundles
    add_library(GfxPluginMDI MODULE ${SOURCES} ${HEADERS})
    set_target_properties(GfxPluginMDI PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "bundle"
        MACOSX_BUNDLE_BUNDLE_NAME "GfxPluginMDI"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.saivs.gfxpluginmdi"
        OUTPUT_NAME "GfxPluginMDI"
    )
    if (NOT CMAKE_OSX_ARCHITECTURES)
        set_target_properties(GfxPluginMDI PROPERTIES OSX_ARCHITECTURES "arm64;x86_64")
    endif()
    set_source_files_properties(src/MDIBackend_Metal.mm PROPERTIES
        COMPILE_FLAGS "-fno-objc-arc"
    )
    target_link_libraries(GfxPluginMDI PRIVATE
        "-framework Foundation"
        "-framework Metal"
    )
else()
    add_library(GfxPluginMDI SHARED ${SOURCES} ${HEADERS})
endif()

target_include_directories(GfxPluginMDI PRIVATE src)

# MDI_DEBUG_LOG is enabled for Debug configs unconditionally, plus whenever
# the user passes -DMDI_DEBUG_LOG=ON regardless of config.
target_compile_definitions(GfxPluginMDI PRIVATE $<$<CONFIG:Debug>:MDI_DEBUG_LOG>)
if (MDI_DEBUG_LOG)
    target_compile_definitions(GfxPluginMDI PRIVATE MDI_DEBUG_LOG)
endif()

if (MSVC)
    # Static CRT — eliminates VCRUNTIME140.dll dependency
    set_property(TARGET GfxPluginMDI PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if (WIN32)
    target_link_libraries(GfxPluginMDI PRIVATE d3d11 d3d12 dxgi)
endif()

if (UNIX AND NOT APPLE AND NOT ANDROID)
    # Desktop Linux: Unity loads libGfxPluginMDI.so via DllImport("GfxPluginMDI").
    # CMAKE_DL_LIBS is needed for dlopen/dlsym on glibc < 2.34.
    target_link_libraries(GfxPluginMDI PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(GfxPluginMDI PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
    # Static C++ runtime — the .so must not depend on the system libstdc++ version.
    # --exclude-libs,ALL keeps the statically linked libstdc++/libgcc symbols private.
    target_link_options(GfxPluginMDI PRIVATE
        -static-libstdc++ -static-libgcc
        -Wl,--exclude-libs,ALL -Wl,--no-undefined)
endif()

if (ANDROID)
    # Unity loads Android native plugins as libGfxPluginMDI.so per ABI.
    # Vulkan/GLES backends are header-only — symbols are resolved at runtime via
    # vkGetDeviceProcAddr / eglGetProcAddress, so we don't link Vulkan/GLES libs here.
    # libc++_shared is explicit because clang++.exe on a path with spaces resolves
    # to a DOS 8.3 short name (`CLANG_~1.EXE`) that drops the `++`, making the driver
    # act as the C variant and skip auto-linking the C++ STL. Unity ships
    # libc++_shared.so in every Android build, so this adds no APK overhead.
    target_link_libraries(GfxPluginMDI PRIVATE log android c++_shared)
    # Hide non-exported symbols and strip — keeps the .so small.
    target_compile_options(GfxPluginMDI PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)
    set_target_properties(GfxPluginMDI PROPERTIES
        LINK_FLAGS "-Wl,--build-id=none -Wl,--gc-sections -Wl,--strip-all"
    )
endif()

# Output to package Plugins folder
get_filename_component(PLUGINS_ROOT "${CMAKE_SOURCE_DIR}/../Plugins" ABSOLUTE)

if (APPLE)
    set(PLUGINS_DIR "${PLUGINS_ROOT}/macOS")
    add_custom_command(TARGET GfxPluginMDI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGINS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${PLUGINS_DIR}/GfxPluginMDI.bundle"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "$<TARGET_BUNDLE_DIR:GfxPluginMDI>"
            "${PLUGINS_DIR}/GfxPluginMDI.bundle"
        COMMENT "Copying GfxPluginMDI.bundle to package Plugins/macOS folder"
    )
elseif (ANDROID)
    set(PLUGINS_DIR "${PLUGINS_ROOT}/Android/${ANDROID_ABI}")
    add_custom_command(TARGET GfxPluginMDI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGINS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:GfxPluginMDI>"
            "${PLUGINS_DIR}/"
        COMMENT "Copying libGfxPluginMDI.so to package Plugins/Android/${ANDROID_ABI} folder"
    )
else()
    set(PLUGINS_DIR "${PLUGINS_ROOT}/x86_64")
    add_custom_command(TARGET GfxPluginMDI POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PLUGINS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:GfxPluginMDI>"
            "${PLUGINS_DIR}/"
        COMMENT "Copying GfxPluginMDI to package Plugins folder"
    )
endif()
