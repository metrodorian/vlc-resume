cmake_minimum_required(VERSION 3.16)
project(vlc-resume C)

# --- Locate VLC plugin headers ------------------------------------------------
# Try pkg-config first, fall back to a manual search.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(VLC QUIET vlc-plugin)
endif()

if(NOT VLC_INCLUDE_DIRS)
    # Common paths on macOS (Homebrew / official .app bundle)
    find_path(VLC_INCLUDE_DIRS
        NAMES vlc_common.h
        PATHS
            /Applications/VLC.app/Contents/MacOS/include
            /usr/local/include/vlc
            /opt/homebrew/include/vlc
            /usr/include/vlc
        PATH_SUFFIXES vlc plugins
    )
endif()

if(NOT VLC_INCLUDE_DIRS)
    message(FATAL_ERROR
        "VLC plugin headers not found.\n"
        "Install the VLC development package or set -DVLC_INCLUDE_DIRS=<path>.")
endif()

message(STATUS "VLC include dirs: ${VLC_INCLUDE_DIRS}")

# --- Plugin target ------------------------------------------------------------
add_library(vlc_resume MODULE
    src/plugin.c
    src/events.c
    src/timer.c
    src/state.c
    src/cJSON.c
)

set_target_properties(vlc_resume PROPERTIES
    PREFIX ""            # VLC expects libvlc_resume.dylib / .so without extra prefix
    OUTPUT_NAME "vlc_resume"
    C_STANDARD 11
)

target_include_directories(vlc_resume PRIVATE
    ${VLC_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(vlc_resume PRIVATE
    HAVE_CONFIG_H=0
    # Silence cJSON's own malloc override when building as part of a plugin
    cJSON_HIDE_SYMBOLS
)

target_compile_options(vlc_resume PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fvisibility=hidden
)

# On macOS plugins are .dylib; tell the linker not to require all symbols
# (VLC provides them at load time).
if(APPLE)
    target_link_options(vlc_resume PRIVATE -undefined dynamic_lookup)
endif()

# --- Install target -----------------------------------------------------------
# Detect VLC plugin directory automatically when possible.
if(NOT VLC_PLUGIN_DIR)
    if(APPLE)
        set(VLC_PLUGIN_DIR "$ENV{HOME}/Library/Application Support/org.videolan.vlc/plugins"
            CACHE PATH "VLC user plugin directory")
    elseif(WIN32)
        set(VLC_PLUGIN_DIR "$ENV{APPDATA}/vlc/plugins"
            CACHE PATH "VLC user plugin directory")
    else()
        set(VLC_PLUGIN_DIR "$ENV{HOME}/.local/lib/vlc/plugins"
            CACHE PATH "VLC user plugin directory")
    endif()
endif()

install(TARGETS vlc_resume
    LIBRARY DESTINATION "${VLC_PLUGIN_DIR}/misc"
)

message(STATUS "Plugin will be installed to: ${VLC_PLUGIN_DIR}/misc")
