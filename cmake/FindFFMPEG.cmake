#[=======================================================================[.rst:
FindFFMPEG
----------

Find FFmpeg multimedia libraries.

Imported targets
^^^^^^^^^^^^^^^^

This module defines the following ``IMPORTED`` targets:

``FFMPEG::avcodec``
``FFMPEG::avformat``
``FFMPEG::avutil``
``FFMPEG::swscale``
``FFMPEG::swresample``

Result variables
^^^^^^^^^^^^^^^^

``FFMPEG_FOUND``
  True if all required components were found.
``FFMPEG_INCLUDE_DIRS``
  Include directories for all found components.
``FFMPEG_LIBRARIES``
  Libraries for all found components.
``FFMPEG_<component>_FOUND``
  True if component was found.
``FFMPEG_<component>_INCLUDE_DIR``
  Include directory for component.
``FFMPEG_<component>_LIBRARY``
  Library for component.

Components: ``avcodec``, ``avformat``, ``avutil``, ``swscale``, ``swresample``
#]=======================================================================]

include(FindPackageHandleStandardArgs)

# Use pkg-config if available
find_package(PkgConfig QUIET)

set(_FFMPEG_COMPONENTS
    avcodec
    avformat
    avutil
    swscale
    swresample
)

# If no components specified, assume all
if(NOT FFMPEG_FIND_COMPONENTS)
    set(FFMPEG_FIND_COMPONENTS ${_FFMPEG_COMPONENTS})
endif()

foreach(comp ${FFMPEG_FIND_COMPONENTS})
    if(PkgConfig_FOUND)
        pkg_check_modules(PC_FFMPEG_${comp} QUIET lib${comp})
    endif()

    find_path(FFMPEG_${comp}_INCLUDE_DIR
        NAMES
            lib${comp}/${comp}.h
        HINTS
            ${PC_FFMPEG_${comp}_INCLUDE_DIRS}
            ${PC_FFMPEG_${comp}_INCLUDEDIR}
        PATHS
            /usr/include
            /usr/local/include
            /opt/homebrew/include
            /opt/local/include
        PATH_SUFFIXES
            ffmpeg
    )

    find_library(FFMPEG_${comp}_LIBRARY
        NAMES
            ${comp}
            ${comp}${CMAKE_STATIC_LIBRARY_SUFFIX}
        HINTS
            ${PC_FFMPEG_${comp}_LIBRARY_DIRS}
            ${PC_FFMPEG_${comp}_LIBDIR}
        PATHS
            /usr/lib
            /usr/local/lib
            /opt/homebrew/lib
            /opt/local/lib
    )

    if(FFMPEG_${comp}_INCLUDE_DIR AND FFMPEG_${comp}_LIBRARY)
        set(FFMPEG_${comp}_FOUND TRUE)
        mark_as_advanced(FFMPEG_${comp}_INCLUDE_DIR FFMPEG_${comp}_LIBRARY)
    endif()

    if(FFMPEG_${comp}_FOUND)
        list(APPEND FFMPEG_INCLUDE_DIRS ${FFMPEG_${comp}_INCLUDE_DIR})
        list(APPEND FFMPEG_LIBRARIES    ${FFMPEG_${comp}_LIBRARY})

        if(NOT TARGET FFMPEG::${comp})
            add_library(FFMPEG::${comp} UNKNOWN IMPORTED)
            set_target_properties(FFMPEG::${comp} PROPERTIES
                IMPORTED_LOCATION             "${FFMPEG_${comp}_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_${comp}_INCLUDE_DIR}"
            )
        endif()
    endif()
endforeach()

if(FFMPEG_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIRS)
endif()

find_package_handle_standard_args(FFMPEG
    REQUIRED_VARS   FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES
    HANDLE_COMPONENTS
)

mark_as_advanced(FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES)
