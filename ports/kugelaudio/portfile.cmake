# Overlay port for the KugelAudio C++ SDK:
#   vcpkg install kugelaudio --overlay-ports=<cpp-sdk checkout>/ports
#
# The source is the release tarball the mirror's GitHub release carries. Its SHA512 is
# filled in when the release is cut (the port files attached to that release carry the
# real hash; this committed copy keeps the placeholder because the tarball contains it).
vcpkg_download_distfile(ARCHIVE
    URLS "https://github.com/Kugelaudio/cpp-sdk/releases/download/v${VERSION}/kugelaudio-cpp-${VERSION}.tar.gz"
    FILENAME "kugelaudio-cpp-${VERSION}.tar.gz"
    SHA512 @KUGELAUDIO_SHA512@
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DKUGELAUDIO_BUILD_TESTS=OFF
        -DKUGELAUDIO_BUILD_EXAMPLES=OFF
        -DKUGELAUDIO_FETCH_DEPS=OFF
        -DKUGELAUDIO_INSTALL=ON
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME kugelaudio CONFIG_PATH lib/cmake/kugelaudio)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
