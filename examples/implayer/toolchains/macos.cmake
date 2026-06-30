# ---------------------------------------------------------------------------
# macOS arm64 native build — static linking of vcpkg dependencies
#
# Loaded via VCPKG_CHAINLOAD_TOOLCHAIN_FILE in CMakePresets.json.
# Sets platform identifier, triplet, and compiler flags.
# ---------------------------------------------------------------------------

set(IMV_PLATFORM "macos" CACHE STRING "Platform identifier (drives toolchains/<id>-link.cmake)")

# ---- vcpkg triplet (arm64-osx already uses VCPKG_LIBRARY_LINKAGE static) ---
set(VCPKG_TARGET_TRIPLET "arm64-osx" CACHE STRING "vcpkg triplet")

# ---- force static linking of project dependencies --------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static linking of all deps")
# macOS CMake defaults to .dylib before .a — flip it so .a wins.
set(CMAKE_FIND_LIBRARY_SUFFIXES .a .dylib)

# ---- silence OpenGL deprecation --------------------------------------------
set(CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT} -DGL_SILENCE_DEPRECATION")
set(CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT} -DGL_SILENCE_DEPRECATION")
