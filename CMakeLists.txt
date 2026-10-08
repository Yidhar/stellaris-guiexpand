cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
project(StellarisGuiExpand LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The static C runtime, as the launcher's plugin handbook recommends (no VC++ redistributable on the player's machine). MinHook and ImGui below
# inherit it. Nothing owned by one plugin is freed by another: the public interface passes no ownership across the DLL boundary.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

option(GUIEXPAND_BUILD_EXAMPLES "Build the example plugins (examples/)" ON)

include(FetchContent)

# MinHook: the inline hooks of ImGui::NewFrame, NImGuiWrapper::ImGuiInit and CGameState::HandleTurnTick
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4
)
# Dear ImGui 1.85: the version compiled into stellaris.exe (4.5.2). Plugins that draw into the engine's context have to be the same version with the
# default imconfig.h; src/imgui_host.cpp has static_asserts against the layout constants of the SDK.
# (offline or a local clone: -DFETCHCONTENT_SOURCE_DIR_IMGUI=<path to an ImGui v1.85 checkout>)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.85
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(minhook imgui)
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp)

# ---- stellaris_guiexpand.dll: the host
add_library(stellaris_guiexpand SHARED
    src/core.cpp
    src/loc.cpp
    src/host_api.cpp
    src/decl_panels.cpp
    src/deck.cpp
    src/imgui_host.cpp
    src/dllmain.cpp
    ${IMGUI_SOURCES})
target_include_directories(stellaris_guiexpand PRIVATE src include ${CMAKE_SOURCE_DIR} ${imgui_SOURCE_DIR})
target_link_libraries(stellaris_guiexpand PRIVATE minhook)
target_compile_definitions(stellaris_guiexpand PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
if (MSVC)
    target_compile_options(stellaris_guiexpand PRIVATE /utf-8)
endif()
set_target_properties(stellaris_guiexpand PROPERTIES PREFIX "")

# The plugin folder as the launcher expects it (docs/PLUGINS.md of the launcher): stl-plugin.json, the DLL, defaults\.
# build\plugin\stellaris-guiexpand can be installed or linked with `stl plugin install [--link] build\plugin\stellaris-guiexpand`.
set(GUIEXPAND_PLUGIN_DIR "${CMAKE_BINARY_DIR}/plugin/stellaris-guiexpand")
add_custom_target(stellaris_guiexpand_plugin ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GUIEXPAND_PLUGIN_DIR}/defaults"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:stellaris_guiexpand>" "${GUIEXPAND_PLUGIN_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/plugin/stl-plugin.json" "${GUIEXPAND_PLUGIN_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/plugin/defaults/stellaris_guiexpand.ini" "${GUIEXPAND_PLUGIN_DIR}/defaults/"
    DEPENDS stellaris_guiexpand
    COMMENT "Assembling ${GUIEXPAND_PLUGIN_DIR}")

# ---- example plugins: what another developer's plugin looks like (docs/developers.md)
if (GUIEXPAND_BUILD_EXAMPLES)
    # a plugin with its own Dear ImGui (same version, same configuration as the engine's)
    add_library(example_imgui SHARED examples/cpp_imgui/consumer_imgui.cpp ${IMGUI_SOURCES})
    target_include_directories(example_imgui PRIVATE include include/stellaris_guiexpand ${imgui_SOURCE_DIR})
    target_compile_definitions(example_imgui PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS CONSUMER_TAG="imgui")

    # the same plugin built with 32-bit draw indices: it must refuse to draw instead of corrupting the shared context
    add_library(example_imgui_badcfg SHARED examples/cpp_imgui/consumer_imgui.cpp ${IMGUI_SOURCES})
    target_include_directories(example_imgui_badcfg PRIVATE include include/stellaris_guiexpand ${imgui_SOURCE_DIR})
    target_compile_definitions(example_imgui_badcfg PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS "ImDrawIdx=unsigned int" CONSUMER_TAG="badcfg")

    # a plugin without any ImGui: plain C through StlGuiUi
    add_library(example_c SHARED examples/c/consumer_c.c)
    target_include_directories(example_c PRIVATE include include/stellaris_guiexpand)
    target_compile_definitions(example_c PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

    # a plugin that registers elements for declared panels (a component library in small): plain C
    add_library(example_element SHARED examples/element/example_element.c)
    target_include_directories(example_element PRIVATE include include/stellaris_guiexpand)
    target_compile_definitions(example_element PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

    foreach(t example_imgui example_imgui_badcfg example_c example_element)
        set_target_properties(${t} PROPERTIES PREFIX "")
    endforeach()
    if (MSVC)
        target_compile_options(example_imgui PRIVATE /utf-8)
    endif()
endif()
