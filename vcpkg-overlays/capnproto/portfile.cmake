# Overlay of the stock vcpkg capnproto port. The only deltas from the registry
# port are the iOS/Android cross-compile guards: the capnp/capnpc-* code
# generators cannot run on the target device (and would need BUNDLE DESTINATION
# on iOS), so mobile builds use EXTERNAL_CAPNP + CAPNP_LITE and skip the tools.
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO capnproto/capnproto
    REF "v${VERSION}"
    SHA512 d3072f590212d40010fa7946e000ac9fe927c9058fcda518c14275c7a217207db644d44a124398873d3875bb5f1f8e52dbeccfbc4b4c003e8e35fd83486fc343
    HEAD_REF master
    PATCHES
        001-fix-android.patch
        002-fix-pkg-config.patch
)

if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW)
    # In ARM64 it fails without /bigobj
    set(VCPKG_CXX_FLAGS "${VCPKG_CXX_FLAGS} /bigobj")
    set(VCPKG_C_FLAGS "${VCPKG_C_FLAGS} /bigobj")
endif()

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        "openssl" OPENSSL_FEATURE
)

# iOS/Android cross-compilation: skip tools (capnp, capnpc-*). They would need
# BUNDLE DESTINATION and can't run on device anyway; the host-triplet build of
# this same port supplies the code generators.
set(EXTERNAL_CAPNP_OPTION "")
if(VCPKG_TARGET_IS_IOS OR VCPKG_TARGET_IS_ANDROID)
    set(EXTERNAL_CAPNP_OPTION "-DEXTERNAL_CAPNP=ON" "-DCAPNP_LITE=ON")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_TESTING=OFF
        "-DWITH_OPENSSL=${OPENSSL_FEATURE}"
        ${EXTERNAL_CAPNP_OPTION}
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/CapnProto)

# Only copy tools for non-mobile platforms
if(NOT VCPKG_TARGET_IS_IOS AND NOT VCPKG_TARGET_IS_ANDROID)
    vcpkg_copy_tools(TOOL_NAMES capnp capnpc-c++ capnpc-capnp AUTO_CLEAN)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/bin")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

# Handle copyright
file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

vcpkg_fixup_pkgconfig()
