vcpkg_minimum_required(VERSION 2024-01-01)

# imvideo vcpkg port
# Source: https://github.com/touken928/imvideo

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO touken928/imvideo
    REF v0.1.0
    SHA512 0
    HEAD_REF main
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DIMVIDEO_BUILD_DEMO=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME imvideo CONFIG_PATH lib/cmake/imvideo)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
