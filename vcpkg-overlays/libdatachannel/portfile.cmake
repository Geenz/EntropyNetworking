# Overlay of the stock vcpkg libdatachannel port. The only deltas from the
# registry port are for iOS: vcpkg's usrsctp 0.9.5.0 does not build there
# (missing <net/route.h>, IPV6_PKTINFO), so iOS uses libdatachannel's bundled
# usrsctp fork (carries the iOS fixes from PR #478) instead of the system
# port. dependencies.diff guards the exported find_dependency(usrsctp) on
# USE_SYSTEM_USRSCTP, and vcpkg.json excludes usrsctp on iOS.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO paullouisageneau/libdatachannel
    REF "v${VERSION}"
    SHA512 694561ba5b3e08ed35e7e167330d97455ee2ef8d298c9c41e12de4d07032bbe1bb2ebec1e35126187c57ef9492e7d1c82ffd0fa3511eabcdfb77efabfd4b7d9a
    HEAD_REF master
    PATCHES
        dependencies.diff
        uwp-warnings.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        stdcall CAPI_STDCALL
    INVERTED_FEATURES
        ws      NO_WEBSOCKET
        srtp    NO_MEDIA
)

# On iOS, usrsctp 0.9.5.0 is broken (missing <net/route.h>, IPV6_PKTINFO).
# libdatachannel's bundled usrsctp fork has all iOS fixes (PR #478).
# Fetch it into deps/usrsctp since vcpkg tarballs don't include submodules.
if(VCPKG_TARGET_IS_IOS)
    vcpkg_from_github(
        OUT_SOURCE_PATH USRSCTP_SOURCE_PATH
        REPO paullouisageneau/usrsctp
        REF fec583d54493f879d2ae44a743423bf8a04371ab
        SHA512 f98ab4945e7a200b54c0139614c730e4ff4b8fd69e38f0e9905e994133a07f614651126f4dc25ca626b58d4584dfaf2eb8266864f826dfd0c8e48567d03a2315
        HEAD_REF master
    )
    file(REMOVE_RECURSE "${SOURCE_PATH}/deps/usrsctp")
    file(RENAME "${USRSCTP_SOURCE_PATH}" "${SOURCE_PATH}/deps/usrsctp")

    vcpkg_cmake_configure(
        SOURCE_PATH "${SOURCE_PATH}"
        OPTIONS
            ${FEATURE_OPTIONS}
            -DUSE_SYSTEM_USRSCTP=OFF
            -DUSE_SYSTEM_JUICE=ON
            -DUSE_SYSTEM_PLOG=ON
            -DUSE_SYSTEM_JSON=ON
            -DUSE_SYSTEM_SRTP=ON
            -DNO_EXAMPLES=ON
            -DNO_TESTS=ON
    )
else()
    vcpkg_cmake_configure(
        SOURCE_PATH "${SOURCE_PATH}"
        OPTIONS
            ${FEATURE_OPTIONS}
            -DPREFER_SYSTEM_LIB=ON
            -DNO_EXAMPLES=ON
            -DNO_TESTS=ON
    )
endif()

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/LibDataChannel)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/rtc/common.hpp" "#ifdef RTC_STATIC" "#if 1")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/rtc/rtc.h" "#ifdef RTC_STATIC" "#if 1")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
