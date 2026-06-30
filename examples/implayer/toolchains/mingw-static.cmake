# ---------------------------------------------------------------------------
# MinGW x86_64 → Windows cross-compilation (static linking)
#
# Loaded via VCPKG_CHAINLOAD_TOOLCHAIN_FILE in CMakePresets.json.
# Requires:  brew install mingw-w64
# ---------------------------------------------------------------------------

# ---- auto-detect MinGW compiler (no hardcoded version paths) ---------------
find_program(_MINGW_GCC x86_64-w64-mingw32-gcc
    HINTS /opt/homebrew/bin
    PATHS /opt/homebrew/Cellar/mingw-w64
)

if(NOT _MINGW_GCC)
    # Homebrew installs into versioned directories — glob the latest one
    file(GLOB _MINGW_CANDIDATES
        /opt/homebrew/Cellar/mingw-w64/*/toolchain-x86_64/bin/x86_64-w64-mingw32-gcc)
    if(_MINGW_CANDIDATES)
        list(GET _MINGW_CANDIDATES 0 _MINGW_GCC)
    endif()
endif()

if(NOT _MINGW_GCC)
    message(FATAL_ERROR
        "x86_64-w64-mingw32-gcc not found.\n"
        "Install with:  brew install mingw-w64\n"
        "Or ensure the bin directory is in PATH.")
endif()

get_filename_component(_MINGW_BIN "${_MINGW_GCC}" DIRECTORY)
get_filename_component(_MINGW_PREFIX "${_MINGW_BIN}" DIRECTORY)

# ---- platform identity ----------------------------------------------------
set(IMV_PLATFORM "mingw-static" CACHE STRING "Platform identifier (drives toolchains/<id>-link.cmake)")

# ---- cross-compilation target ---------------------------------------------
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   "${_MINGW_GCC}")
set(CMAKE_CXX_COMPILER "${_MINGW_BIN}/x86_64-w64-mingw32-g++")
set(CMAKE_RC_COMPILER  "${_MINGW_BIN}/x86_64-w64-mingw32-windres")

# ---- sysroot (derived from compiler location) -----------------------------
set(CMAKE_FIND_ROOT_PATH "${_MINGW_PREFIX}/x86_64-w64-mingw32")

# vcpkg installs libraries outside the sysroot; use BOTH so CMake
# can find them while still preferring the sysroot for crt headers.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# ---- vcpkg triplet --------------------------------------------------------
set(VCPKG_TARGET_TRIPLET "x64-mingw-static" CACHE STRING "vcpkg triplet")
set(VCPKG_OVERLAY_TRIPLETS "$ENV{VCPKG_ROOT}/triplets/community" CACHE STRING "")

# ---- static linking + size optimization -----------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static linking")
set(CMAKE_MSVC_RUNTIME_LIBRARY "")

# Release: optimize for size (-Os) rather than speed (-O3).
# Section-splitting lets --gc-sections discard unreferenced code.
set(CMAKE_C_FLAGS_RELEASE_INIT "-Os -DNDEBUG -ffunction-sections -fdata-sections")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-Os -DNDEBUG -ffunction-sections -fdata-sections")

# Strip debug symbols, discard dead sections, suppress console.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-static -static-libgcc -static-libstdc++ -mwindows -s -Wl,--gc-sections")

# Disable the powershell post-build step (cross-compiling from non-Windows)
set(VCPKG_APPLOCAL_DEPS OFF CACHE BOOL "" FORCE)

# Clean up internal vars
unset(_MINGW_GCC)
unset(_MINGW_BIN)
unset(_MINGW_PREFIX)
unset(_MINGW_CANDIDATES)
