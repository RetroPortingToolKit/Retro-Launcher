# Retro Launcher — multi-title hub for recomp/decomp projects.
#
#   cmake -G Ninja -S . -B build
#   cmake --build build -j
#   ./build/retcomm list
#   ./build/retcomm-hub

cmake_minimum_required(VERSION 3.24)
project(retcomm_launcher C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# macOS: leave room in Mach-O load commands so packaging can rewrite Homebrew
# install names to @rpath / @loader_path without install_name_tool failing.
if(APPLE)
    add_link_options(-Wl,-headerpad_max_install_names)
endif()

# Embedded in binaries for Update Retro / about text.
set(RETCOMM_VERSION "0.1.1" CACHE STRING "Retro Launcher semantic version")
set(RETCOMM_GITHUB_SLUG "RetroPortingToolKit/Retro-Launcher" CACHE STRING
    "GitHub owner/repo used for self-update")
set(RETCOMM_CATALOG_GITHUB_REPO "RetroPortingToolKit/Retro-Catalog" CACHE STRING
    "GitHub owner/repo for the title catalog")
set(RETCOMM_CATALOG_DOWNLOAD_URL
    "https://github.com/RetroPortingToolKit/Retro-Catalog/releases/latest/download/catalog.zip"
    CACHE STRING "Default catalog.zip download URL")

# Portable installs: look next to the binary first; baked path is a dev fallback.
find_package(CURL REQUIRED)

set(RETCOMM_CORE_SOURCES
    src/paths/paths.cpp
    src/paths/data_root.cpp
    src/paths/data_root_migrate.cpp
    src/paths/process_env.cpp
    src/paths/toolchain_env.cpp
    src/core/hash.cpp
    src/core/config.cpp
    src/core/app_state.cpp
    src/core/texture_packs.cpp
    src/core/mods.cpp
    src/catalog/catalog.cpp
    src/catalog/catalog_sync.cpp
    src/romscan/romscan.cpp
    src/library/library_index.cpp
    src/bios/bios_index.cpp
    src/install/http.cpp
    src/install/install.cpp
    src/install/release_tags.cpp
    src/build/build.cpp
    src/build/cache_gc.cpp
    src/build/disc_stage.cpp
    src/update/self_update.cpp
    src/launch/launch.cpp
    src/core/image_compose.cpp
    src/steam/steam_shortcut.cpp
    src/platform/psx_platform_settings.cpp
    src/platform/psx_input_profiles.cpp
    src/platform/snes_platform_settings.cpp
    src/platform/platform_settings.cpp
    src/romm/romm.cpp
    src/romm/romm_fetch.cpp
    src/romm/romm_saves.cpp
    src/netplay/netplay_ws.cpp
    src/netplay/netplay_client.cpp
    src/netplay/netplay_account.cpp
)

# --- miniz (vendored) -------------------------------------------------------
# In-process zip reading for retcomm_core (downloaded archives) and for the
# Windows portable stub (its appended payload). The stub cannot use a shared
# zlib -- the DLLs it would link against are inside the payload it is
# unpacking -- so this is static and dependency-free.
#
# The defines are PUBLIC, not PRIVATE: include/retcomm/zip_extract.hpp includes
# miniz.h in every consumer, and a consumer that saw a different configuration
# than miniz.c was built with would be an ODR violation. Compression and the
# zlib-style API are off; MINIZ_NO_ZLIB_COMPATIBLE_NAMES keeps miniz from
# defining compress()/crc32()/inflate() and colliding with the real zlib that
# libcurl already pulls in.
add_library(retcomm_miniz STATIC third_party/miniz/miniz.c)
target_include_directories(retcomm_miniz PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz)
target_compile_definitions(retcomm_miniz PUBLIC
    MINIZ_NO_DEFLATE_APIS
    MINIZ_NO_ZLIB_APIS
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES
    MINIZ_NO_STDIO
)
set_target_properties(retcomm_miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(retcomm_core STATIC ${RETCOMM_CORE_SOURCES})
target_include_directories(retcomm_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    # stb_image/stb_image_write: implemented once in src/core/image_compose.cpp,
    # included for declarations by the hub's boxart cache. PUBLIC so both see
    # the same header.
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)
target_compile_definitions(retcomm_core PUBLIC
    "RETCOMM_VERSION=\"${RETCOMM_VERSION}\""
    "RETCOMM_GITHUB_SLUG=\"${RETCOMM_GITHUB_SLUG}\""
    "RETCOMM_CATALOG_GITHUB_REPO=\"${RETCOMM_CATALOG_GITHUB_REPO}\""
    "RETCOMM_CATALOG_DOWNLOAD_URL=\"${RETCOMM_CATALOG_DOWNLOAD_URL}\""
)
target_link_libraries(retcomm_core PUBLIC CURL::libcurl retcomm_miniz)
if(WIN32)
    # ws2_32 for WSAPoll in src/netplay/netplay_ws.cpp. PUBLIC because the
    # symbol is in this library, so every consumer needs it — retcomm-hub only
    # linked without it by accident, SDL3 happening to drag winsock in, while
    # retcomm.exe (no SDL) failed with an unresolved __imp_WSAPoll.
    target_link_libraries(retcomm_core PUBLIC shell32 ws2_32)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(retcomm_core PUBLIC stdc++fs)
endif()

# --- Windows resources (icon + VERSIONINFO) ---------------------------------
# Each exe gets its own .rc so InternalName / OriginalFilename / FileDescription
# name that specific binary. Version metadata matters beyond tidiness: a file
# carrying none gives SmartScreen nothing to attribute a download to and scores
# higher in Defender's ML heuristics. See packaging/windows/retcomm.rc.in.
set(RETCOMM_RC_COMPANY "TechnicallyComputers")
set(RETCOMM_RC_PRODUCT "RetComM Launcher")
set(RETCOMM_RC_COPYRIGHT "Copyright (C) TechnicallyComputers. MIT licensed.")

# FILEVERSION wants four comma-separated integers; RETCOMM_VERSION may carry a
# prerelease suffix (0.6.4-rc1), so take only the leading x.y.z.
if(RETCOMM_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(RETCOMM_VERSION_COMMA "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},0")
else()
    message(WARNING "RETCOMM_VERSION '${RETCOMM_VERSION}' is not x.y.z; "
                    "Windows FILEVERSION falls back to 0,0,0,0")
    set(RETCOMM_VERSION_COMMA "0,0,0,0")
endif()

# retcomm_add_windows_rc(<out_var> <internal_name> <original_filename> <description>)
# Sets <out_var> to the generated .rc, or to "" off Windows / with no icon, so
# callers can splice it into add_executable() unconditionally.
function(retcomm_add_windows_rc out_var internal_name original_filename description)
    set(${out_var} "" PARENT_SCOPE)
    if(NOT WIN32 OR NOT EXISTS "${CMAKE_SOURCE_DIR}/assets/retcomm.ico")
        return()
    endif()
    string(REPLACE "\\" "/" RETCOMM_ICO_PATH "${CMAKE_SOURCE_DIR}/assets/retcomm.ico")
    set(RETCOMM_RC_INTERNAL_NAME "${internal_name}")
    set(RETCOMM_RC_ORIGINAL_FILENAME "${original_filename}")
    set(RETCOMM_RC_DESCRIPTION "${description}")
    set(rc_path "${CMAKE_BINARY_DIR}/rc/${internal_name}.rc")
    configure_file("${CMAKE_SOURCE_DIR}/packaging/windows/retcomm.rc.in" "${rc_path}" @ONLY)
    set(${out_var} "${rc_path}" PARENT_SCOPE)
endfunction()

# OriginalFilename is the name each file actually ships under: package.ps1
# renames the portable stub to "RetComM Launcher.exe".
retcomm_add_windows_rc(RETCOMM_RC_CLI "retcomm" "retcomm.exe"
    "RetComM Launcher command line")
retcomm_add_windows_rc(RETCOMM_RC_PORTABLE "retcomm-portable" "RetComM Launcher.exe"
    "RetComM Launcher (portable)")
retcomm_add_windows_rc(RETCOMM_RC_HUB "retcomm-hub" "retcomm-hub.exe"
    "RetComM Launcher")

add_executable(retcomm src/main.cpp ${RETCOMM_RC_CLI})
target_link_libraries(retcomm PRIVATE retcomm_core)
if(UNIX AND NOT APPLE)
    set_target_properties(retcomm PROPERTIES INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN")
endif()

# Windows portable stub (payload appended at package time).
if(WIN32)
    add_executable(retcomm-portable WIN32
        src/portable/win_portable_main.cpp
        ${RETCOMM_RC_PORTABLE}
    )
    # Unpacks its payload with the shared reader in include/retcomm/zip_extract.hpp;
    # retcomm_miniz supplies the include dir and defines. Nothing else from core is
    # linked: the stub runs before the payload DLLs exist on disk.
    target_include_directories(retcomm-portable PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(retcomm-portable PRIVATE
        "RETCOMM_VERSION=\"${RETCOMM_VERSION}\""
    )
    # The stub's entry point is wWinMain. MSVC picks it up from the WIN32
    # subsystem; mingw's GUI startup calls WinMain unless -municode selects the
    # wide one (cross builds: cmake/toolchain-mingw-w64-x86_64.cmake).
    if(MINGW)
        target_compile_options(retcomm-portable PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-municode>)
        target_link_options(retcomm-portable PRIVATE -municode)
    endif()
    target_link_libraries(retcomm-portable PRIVATE shell32 retcomm_miniz)
    install(TARGETS retcomm-portable RUNTIME DESTINATION bin)
endif()

# ---------------------------------------------------------------------------
# Hub UI (SDL3 + OpenGL + Dear ImGui)
# ---------------------------------------------------------------------------
# Prefer a sibling recomp-ui checkout; otherwise FetchContent ocornut/imgui.
set(RETCOMM_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../recomp-ui/src/third_party/imgui"
    CACHE PATH "Path to Dear ImGui sources")

find_package(SDL3 QUIET)

# GL for the ImGui SDL3+GL3 backend. Re-running find_package cannot conjure
# OpenGL::GL: FindOpenGL only defines it when a legacy libGL was found, or when
# BOTH OpenGL::OpenGL and OpenGL::GLX exist (GLX needs glx.h). A GLVND host with
# libOpenGL.so but no legacy libGL.so / glx.h — Steam Deck, SteamOS — reports
# OpenGL as *found* with no OpenGL::GL, so REQUIRED passes and the link fails at
# generate time instead. Resolve to whatever the host actually has.
# Canonical version of this logic: recomp-ui/cmake/recomp_gl.cmake (duplicated
# because the launcher must configure without a recomp-ui checkout).
find_package(OpenGL REQUIRED)
if(TARGET OpenGL::GL)
    set(RETCOMM_GL_TARGET OpenGL::GL)
elseif(TARGET OpenGL::OpenGL)
    message(STATUS "retcomm-hub: OpenGL::GL unavailable (GLVND host) — linking OpenGL::OpenGL")
    set(RETCOMM_GL_TARGET OpenGL::OpenGL)
elseif(OPENGL_gl_LIBRARY)
    set(RETCOMM_GL_TARGET "${OPENGL_gl_LIBRARY}")
elseif(OPENGL_opengl_LIBRARY)
    set(RETCOMM_GL_TARGET "${OPENGL_opengl_LIBRARY}")
else()
    message(FATAL_ERROR
        "retcomm-hub: OpenGL found but no usable GL library resolved "
        "(OPENGL_gl_LIBRARY='${OPENGL_gl_LIBRARY}', "
        "OPENGL_opengl_LIBRARY='${OPENGL_opengl_LIBRARY}'). "
        "Install the GL development files (libgl-dev / libglvnd-dev / libglvnd).")
endif()

if(NOT SDL3_FOUND AND NOT TARGET SDL3::SDL3)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL3 IMPORTED_TARGET sdl3)
    endif()
endif()

set(RETCOMM_HAVE_SDL3 FALSE)
set(RETCOMM_HUB_SDL_TARGET "")
if(TARGET SDL3::SDL3)
    set(RETCOMM_HAVE_SDL3 TRUE)
    set(RETCOMM_HUB_SDL_TARGET SDL3::SDL3)
elseif(TARGET PkgConfig::SDL3)
    set(RETCOMM_HAVE_SDL3 TRUE)
    set(RETCOMM_HUB_SDL_TARGET PkgConfig::SDL3)
endif()

set(RETCOMM_IMGUI_READY FALSE)
if(EXISTS "${RETCOMM_IMGUI_DIR}/imgui.cpp"
   AND EXISTS "${RETCOMM_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp")
    set(RETCOMM_IMGUI_READY TRUE)
elseif(RETCOMM_HAVE_SDL3)
    include(FetchContent)
    message(STATUS "retcomm-hub: fetching Dear ImGui (SDL3 backend)")
    FetchContent_Declare(retcomm_imgui_src
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9-docking
        GIT_SHALLOW TRUE
    )
    # Populate only — do not add imgui's own CMake project.
    FetchContent_GetProperties(retcomm_imgui_src)
    if(NOT retcomm_imgui_src_POPULATED)
        FetchContent_Populate(retcomm_imgui_src)
    endif()
    set(RETCOMM_IMGUI_DIR "${retcomm_imgui_src_SOURCE_DIR}" CACHE PATH
        "Path to Dear ImGui sources" FORCE)
    if(EXISTS "${RETCOMM_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp")
        set(RETCOMM_IMGUI_READY TRUE)
    endif()
endif()

if(RETCOMM_HAVE_SDL3 AND RETCOMM_IMGUI_READY)
    # FreeType enables CBDT/CBLC color emoji (Noto Color Emoji) via LoadColor.
    set(RETCOMM_IMGUI_FREETYPE FALSE)
    set(RETCOMM_IMGUI_FREETYPE_TARGET "")
    find_package(Freetype QUIET)
    if(TARGET Freetype::Freetype)
        set(RETCOMM_IMGUI_FREETYPE TRUE)
        set(RETCOMM_IMGUI_FREETYPE_TARGET Freetype::Freetype)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(RETCOMM_FT2 IMPORTED_TARGET freetype2)
            if(TARGET PkgConfig::RETCOMM_FT2)
                set(RETCOMM_IMGUI_FREETYPE TRUE)
                set(RETCOMM_IMGUI_FREETYPE_TARGET PkgConfig::RETCOMM_FT2)
            endif()
        endif()
    endif()

    set(RETCOMM_IMGUI_SOURCES
        ${RETCOMM_IMGUI_DIR}/imgui.cpp
        ${RETCOMM_IMGUI_DIR}/imgui_draw.cpp
        ${RETCOMM_IMGUI_DIR}/imgui_tables.cpp
        ${RETCOMM_IMGUI_DIR}/imgui_widgets.cpp
        ${RETCOMM_IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
        ${RETCOMM_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    )
    # Layout matches upstream imgui: "misc/freetype/imgui_freetype.h" (see imgui_draw.cpp).
    set(RETCOMM_IMGUI_FREETYPE_SRC
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/misc/freetype/imgui_freetype.cpp")
    if(RETCOMM_IMGUI_FREETYPE AND EXISTS "${RETCOMM_IMGUI_FREETYPE_SRC}")
        list(APPEND RETCOMM_IMGUI_SOURCES ${RETCOMM_IMGUI_FREETYPE_SRC})
    else()
        set(RETCOMM_IMGUI_FREETYPE FALSE)
    endif()

    add_library(retcomm_imgui STATIC ${RETCOMM_IMGUI_SOURCES})
    target_include_directories(retcomm_imgui PUBLIC
        ${RETCOMM_IMGUI_DIR}
        ${RETCOMM_IMGUI_DIR}/backends
    )
    target_link_libraries(retcomm_imgui PUBLIC ${RETCOMM_HUB_SDL_TARGET} ${RETCOMM_GL_TARGET})
    if(RETCOMM_IMGUI_FREETYPE)
        # Prefer our vendored misc/ over a trimmed imgui tree that lacks it.
        # third_party → "misc/freetype/…"; misc/freetype → "imgui_freetype.h" in .cpp.
        target_include_directories(retcomm_imgui PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/misc/freetype)
        target_compile_definitions(retcomm_imgui PUBLIC IMGUI_ENABLE_FREETYPE)
        target_link_libraries(retcomm_imgui PUBLIC ${RETCOMM_IMGUI_FREETYPE_TARGET})
        message(STATUS "retcomm-hub: ImGui FreeType color-emoji support enabled")
    else()
        message(STATUS "retcomm-hub: ImGui FreeType not found — color emoji disabled")
    endif()

    # WIN32 → /SUBSYSTEM:WINDOWS so the hub has no console window on Windows.
    # CLI stays console (retcomm.exe). Portable stub is already WIN32.
    if(WIN32)
        add_executable(retcomm-hub WIN32
            src/hub/hub_main.cpp
            src/hub/hub_model.cpp
            src/hub/hub_boxart.cpp
            src/hub/hub_osk.cpp
            ${RETCOMM_RC_HUB}
        )
        # Hub uses int main(); MSVC Windows subsystem defaults to WinMain.
        if(MSVC)
            target_link_options(retcomm-hub PRIVATE "/ENTRY:mainCRTStartup")
        elseif(MINGW)
            # mingw's -mwindows swaps in a startup object that calls WinMain, so
            # keep the console startup (main) and mark the image GUI at link time.
            set_target_properties(retcomm-hub PROPERTIES WIN32_EXECUTABLE FALSE)
            target_link_options(retcomm-hub PRIVATE "-Wl,--subsystem,windows")
        endif()
    else()
        add_executable(retcomm-hub
            src/hub/hub_main.cpp
            src/hub/hub_model.cpp
            src/hub/hub_boxart.cpp
            src/hub/hub_osk.cpp
        )
    endif()
    target_include_directories(retcomm-hub PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
    )
    # Optional: stb also available from a sibling recomp-ui tree.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../recomp-ui/src/third_party/stb_image.h")
        target_include_directories(retcomm-hub PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../recomp-ui/src/third_party)
    endif()
    target_link_libraries(retcomm-hub PRIVATE retcomm_core retcomm_imgui)
    if(UNIX AND NOT APPLE)
        set_target_properties(retcomm-hub PROPERTIES INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN")
        # A core running in the hub's window (docs/CORE_LINK.md). Linux only
        # until the link has a Windows transport.
        target_sources(retcomm-hub PRIVATE src/hub/hub_play.cpp src/corelink/link_io.cpp
                                           src/corelink/core_link.cpp)
        target_compile_definitions(retcomm-hub PRIVATE RETCOMM_HUB_HAVE_PLAY=1)
    endif()
    install(TARGETS retcomm-hub RUNTIME DESTINATION bin)
    message(STATUS "retcomm-hub: enabled (ImGui=${RETCOMM_IMGUI_DIR})")
else()
    message(STATUS "retcomm-hub: skipped (need SDL3 + ImGui with imgui_impl_sdl3)")
endif()

# --- the hub <-> runner link (docs/CORE_LINK.md) ----------------------------
# Linux only for now: memfd, SOCK_SEQPACKET and SCM_RIGHTS. The Windows link
# (named mappings, a named pipe, DuplicateHandle) is not built, and neither is
# the runner there.
if(UNIX AND NOT APPLE)
add_library(retcomm_corelink STATIC
    src/corelink/link_io.cpp
    src/corelink/core_link.cpp
)
target_include_directories(retcomm_corelink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_executable(retcomm-core-link-test src/corelink/link_test_main.cpp)
target_link_libraries(retcomm-core-link-test PRIVATE retcomm_corelink)
if(NOT WIN32 AND NOT APPLE)
    target_link_libraries(retcomm-core-link-test PRIVATE stdc++fs)
endif()

# --- retcomm-core-runner --------------------------------------------------
# The child process that loads an rcore core (docs/HOST_LIFECYCLE.md). It
# links only what it uses -- the SHA-256 for the core's identity, and SDL3 when
# present for lending a GL context -- never retcomm_core and its libcurl: it
# starts once per game and should be small.
add_executable(retcomm-core-runner
    src/runner/runner_main.cpp
    src/runner/runner_link.cpp
    src/runner/core_library.cpp
    src/runner/core_manifest.cpp
    src/runner/host_session.cpp
    src/core/hash.cpp
)
target_link_libraries(retcomm-core-runner PRIVATE retcomm_corelink)
target_include_directories(retcomm-core-runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(retcomm-core-runner PRIVATE ${CMAKE_DL_LIBS})
if(NOT WIN32 AND NOT APPLE)
    target_link_libraries(retcomm-core-runner PRIVATE stdc++fs)
endif()
if(RETCOMM_HAVE_SDL3)
    target_link_libraries(retcomm-core-runner PRIVATE ${RETCOMM_HUB_SDL_TARGET})
    target_compile_definitions(retcomm-core-runner PRIVATE RETCOMM_RUNNER_HAVE_SDL3=1)
endif()
install(TARGETS retcomm-core-runner RUNTIME DESTINATION bin)

# A test fixture, never installed: the smallest core, silent and 50 Hz, so a
# host's frame-rate pacing can be measured (docs/CORE_LINK.md). Its sidecar is
# written beside it; the runner checks it like any other.
add_library(rcore_fake_core SHARED tests/rcore_fake_core.c)
target_include_directories(rcore_fake_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(rcore_fake_core PROPERTIES PREFIX "" OUTPUT_NAME fake_core
                      C_VISIBILITY_PRESET hidden)
file(GENERATE OUTPUT $<TARGET_FILE_DIR:rcore_fake_core>/fake_core.rcore.toml CONTENT
"# Written by CMake for the rcore_fake_core test fixture.\n\n[core]\nabi_major       = 0\ndraft_revision  = 5\nid              = \"fake\"\nversion         = \"1.0\"\nlibrary         = \"fake_core.so\"\nplatforms       = [\"test\"]\ncapabilities    = [\"run_frame\"]\n\n[build]\nengine_commit   = \"\"\nengine_dirty    = false\ntoolchain       = \"\"\ngenerated_utc   = \"\"\n")
endif()

install(TARGETS retcomm RUNTIME DESTINATION bin)
install(FILES assets/retcomm.svg DESTINATION share/icons/hicolor/scalable/apps
        RENAME retcomm.svg)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/retcomm.png")
    install(FILES assets/retcomm.png DESTINATION share/icons/hicolor/512x512/apps
            RENAME retcomm.png)
    install(FILES assets/retcomm.png DESTINATION share/retcomm)
endif()
# Hub UI fonts (Lato Latin — same face as recomp-ui). Required when hub is built.
set(RETCOMM_FONT_REGULAR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/LatoLatin-Regular.ttf")
if(TARGET retcomm-hub AND NOT EXISTS "${RETCOMM_FONT_REGULAR}")
    message(FATAL_ERROR
        "retcomm-hub needs ${RETCOMM_FONT_REGULAR} (and LatoLatin-Bold.ttf). "
        "Commit assets/fonts/ so AppImage/DMG/Windows packages ship the UI face.")
endif()
if(EXISTS "${RETCOMM_FONT_REGULAR}")
    install(FILES
        assets/fonts/LatoLatin-Regular.ttf
        assets/fonts/LatoLatin-Bold.ttf
        assets/fonts/NOTICE.md
        DESTINATION share/retcomm/fonts)
endif()
# Hub platform controller icons (library platform cards).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/platforms/psx.png")
    install(DIRECTORY assets/platforms/
            DESTINATION share/retcomm/platforms
            FILES_MATCHING PATTERN "*.png")
endif()
# PSX DualShock art for the Gamepads configure mapper.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/controllers/pad_analog.png")
    install(DIRECTORY assets/controllers/
            DESTINATION share/retcomm/controllers
            FILES_MATCHING PATTERN "*.png" PATTERN "*.tga")
endif()
# First-run setup path cards (Easy / Advanced).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/setup/setup_easy_rocket.png")
    install(DIRECTORY assets/setup/
            DESTINATION share/retcomm/setup
            FILES_MATCHING PATTERN "*.png")
endif()

# Windows: ship vcpkg/shared runtime DLLs next to the installed exes.
if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:retcomm> DESTINATION bin COMPONENT Runtime)
    if(TARGET retcomm-hub)
        install(FILES $<TARGET_RUNTIME_DLLS:retcomm-hub> DESTINATION bin COMPONENT Runtime)
    endif()
endif()
