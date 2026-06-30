vcpkg_minimum_required(VERSION 2022-01-01)

# imvideo local port — builds from the parent source tree (v2).
set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../../")

vcpkg_configure_cmake(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCMAKE_MODULE_PATH=${CMAKE_CURRENT_LIST_DIR}/cmake
)

vcpkg_install_cmake()

# Clean debug-only artifacts: headers and share are already handled
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/lib/cmake")

if(EXISTS "${SOURCE_PATH}/LICENSE")
    vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
endif()
