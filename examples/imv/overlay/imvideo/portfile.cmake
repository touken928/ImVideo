vcpkg_minimum_required(VERSION 2024-01-01)

# imvideo local port — builds from the parent source tree.
# portfile path:  overlay/imvideo/portfile.cmake
# source root:    ../../../../  (overlay/imvideo/ → imv/ → examples/ → immpv)
set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../../")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DIMVIDEO_BUILD_DEMO=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME imvideo CONFIG_PATH lib/cmake/imvideo)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

if(EXISTS "${SOURCE_PATH}/LICENSE")
    vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
endif()
