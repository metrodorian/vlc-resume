cmake_minimum_required(VERSION 3.16)
project(vlc-resume C)

# --- Locate VLC plugin headers ------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(VLC QUIET vlc-plugin)
endif()

if(NOT VLC_INCLUDE_DIRS)
    find_path(VLC_INCLUDE_DIRS
        NAMES vlc_common.h
        PATHS
            /usr/local/include/vlc
            /opt/homebrew/include/vlc
            /usr/include/vlc
        PATH_SUFFIXES vlc plugins
    )
endif()

if(NOT VLC_INCLUDE_DIRS)
    message(FATAL_ERROR
        "VLC plugin headers not found.\n"
        "Download the VLC 3.0.x source and pass:\n"
        "  cmake -B build -DVLC_INCLUDE_DIRS=/path/to/vlc-src/include")
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

# VLC plugin naming convention: lib<name>_plugin.dylib / lib<name>_plugin.so
set_target_properties(vlc_resume PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "vlc_resume_plugin"
    C_STANDARD 11
)
# VLC on macOS expects .dylib, not .so (CMake MODULE default)
if(APPLE)
    set_target_properties(vlc_resume PROPERTIES SUFFIX ".dylib")
endif()

target_include_directories(vlc_resume PRIVATE
    ${VLC_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_definitions(vlc_resume PRIVATE
    HAVE_CONFIG_H=1
    MODULE_STRING="vlc_resume"
    cJSON_HIDE_SYMBOLS
)

target_compile_options(vlc_resume PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -fvisibility=hidden
)

# VLC provides all symbols at dlopen time — no link-time resolution needed.
if(APPLE)
    target_link_options(vlc_resume PRIVATE -undefined dynamic_lookup)
endif()

# --- Install target -----------------------------------------------------------
if(NOT VLC_PLUGIN_DIR)
    if(APPLE)
        # Write to a user directory; launch VLC with VLC_PLUGIN_PATH set.
        # Avoids modifying the signed VLC.app bundle.
        set(VLC_PLUGIN_DIR "$ENV{HOME}/.local/lib/vlc/plugins"
            CACHE PATH "VLC plugin output directory")
    elseif(WIN32)
        set(VLC_PLUGIN_DIR "$ENV{APPDATA}/vlc/plugins"
            CACHE PATH "VLC plugin output directory")
    else()
        set(VLC_PLUGIN_DIR "$ENV{HOME}/.local/lib/vlc/plugins"
            CACHE PATH "VLC plugin output directory")
    endif()
endif()

install(TARGETS vlc_resume
    LIBRARY DESTINATION "${VLC_PLUGIN_DIR}"
)

message(STATUS "Plugin will be installed to: ${VLC_PLUGIN_DIR}")
