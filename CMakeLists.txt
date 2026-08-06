cmake_minimum_required(VERSION 3.20)
project(NukeTilemap CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Match the rest of the toolchain: dynamic debug CRT (/MDd in Debug).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(NUKE_ROOT       "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(VCPKG_INSTALLED "$ENV{VCPKG_ROOT}/installed/x64-windows")   # VCPKG_ROOT env var, not a hardcoded path

# Reflection prebuild (nukegen module mode): scans the public header for NUKE_CLASS +
# [[nuke::prop/func]] and generates the in-TU registration include.
set(NUKE_GEN "${NUKE_ROOT}/NukeUtils/bin/NukeGen.exe")   # built FIRST by the superbuild
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/src/NukeTilemap.gen.inc"
    COMMAND "${NUKE_GEN}"
            --include "${CMAKE_CURRENT_SOURCE_DIR}/include"
            --out "${CMAKE_CURRENT_SOURCE_DIR}/src/NukeTilemap.gen.inc"
            --init NukeReflectInit_NukeTilemap --no-includes
            --sdk "${NUKE_ROOT}/NukeUtils/sdk/nukesdk/NukeTilemap.sdk.h" --doc "${NUKE_ROOT}/NukeUtils/sdkdocs/NukeTilemap.md"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/NukeTilemap/Tilemap.h" "${NUKE_GEN}"
    COMMENT "nukegen: NukeTilemap reflection")

add_library(NukeTilemap SHARED
    src/NukeTilemap.cpp
    src/Tilemap.cpp
    src/Path.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/src/NukeTilemap.gen.inc"
)
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/NukeTilemap.gen.inc" PROPERTIES HEADER_FILE_ONLY ON)

# Consumer of the engine: NUKEENGINE_API is dllimport, links NukeEngine.lib. Exports its
# OWN public API (NUKETILEMAP_API) so native game modules can link NukeTilemap.lib and
# call the tilemap directly (mapgen fills, A* queries — no reflection on hot paths).
target_compile_definitions(NukeTilemap PRIVATE
    WIN32 _WINDOWS NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
    GLM_ENABLE_EXPERIMENTAL
    BOOST_ALL_DYN_LINK
    NUKETILEMAP_EXPORTS
)

target_include_directories(NukeTilemap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${NUKE_ROOT}/NukeEngine
    ${NUKE_ROOT}/NukeEngine/include
    ${NUKE_ROOT}/NukeEngine/deps
    ${NUKE_ROOT}/NukeEngine/deps/glm
    ${VCPKG_INSTALLED}/include
)

target_link_directories(NukeTilemap PRIVATE
    ${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>            # NukeEngine.lib (import lib)
    ${VCPKG_INSTALLED}/$<$<CONFIG:Debug>:debug/>lib  # boost (auto-linked)
)

target_link_libraries(NukeTilemap PRIVATE
    NukeEngine
)

# Deploy the plugin DLL into the host's modules/ folder (editor + player both load from here).
set(MODULES_DIR "${NUKE_ROOT}/NukeEngine/x64/$<CONFIG>/modules")
add_custom_command(TARGET NukeTilemap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MODULES_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:NukeTilemap>" "${MODULES_DIR}/"
    COMMENT "Deploying NukeTilemap.dll to modules/"
)
