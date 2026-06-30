# Wraps vcpkg's FindFFMPEG.cmake (variable-style output) and creates
# FFMPEG::* imported targets so the library can use target_link_libraries
# with FFMPEG::avcodec etc. directly.
#
# The vcpkg-supplied module only sets FFMPEG_INCLUDE_DIRS, FFMPEG_LIBRARIES,
# and FFMPEG_lib<comp>_LIBRARY (semicolon-separated optimized;…;debug;…).
# We convert those into proper IMPORTED targets.

# Delegate to the vcpkg-installed FindFFMPEG.cmake first.
find_file(_VCPKG_FFMPEG FindFFMPEG.cmake
    PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/ffmpeg"
    NO_DEFAULT_PATH
)
if(_VCPKG_FFMPEG)
    include("${_VCPKG_FFMPEG}")
endif()

if(FFMPEG_FOUND AND NOT TARGET FFMPEG::avcodec)
    # Collect system-library dependencies that FFmpeg needs at link time.
    # The vcpkg FindFFMPEG stores resolved paths in FFMPEG_DEPS_LIBRARY_RELEASE.
    set(_ffmpeg_system_deps "")
    if(DEFINED FFMPEG_DEPS_LIBRARY_RELEASE)
        list(APPEND _ffmpeg_system_deps ${FFMPEG_DEPS_LIBRARY_RELEASE})
    endif()

    foreach(_comp avcodec avformat avutil swscale swresample)
        if(DEFINED FFMPEG_lib${_comp}_LIBRARY_RELEASE)
            add_library(FFMPEG::${_comp} UNKNOWN IMPORTED)
            set_target_properties(FFMPEG::${_comp} PROPERTIES
                IMPORTED_LOCATION_RELEASE "${FFMPEG_lib${_comp}_LIBRARY_RELEASE}"
                IMPORTED_LOCATION_DEBUG   "${FFMPEG_lib${_comp}_LIBRARY_DEBUG}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_lib${_comp}_INCLUDE_DIRS}"
                INTERFACE_LINK_LIBRARIES "${_ffmpeg_system_deps}"
            )
        endif()
    endforeach()
endif()
