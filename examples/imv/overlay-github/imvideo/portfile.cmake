vcpkg_minimum_required(VERSION 2022-01-01)

# imvideo — consume from GitHub via vcpkg_from_github.
#
# NOTE: The SHA512 below is a placeholder.  On first build vcpkg will fail
# with the correct hash.  Copy it from the error message and update here.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO touken928/imvideo
    REF v0.1.0
    SHA512 0
    HEAD_REF main
)

vcpkg_configure_cmake(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DCMAKE_MODULE_PATH=${CMAKE_CURRENT_LIST_DIR}/cmake
)

vcpkg_install_cmake()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/lib/cmake")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
