# vcpkg has no port for QtNodes, so this overlay pins a commit of it
# (docs/MATERIAL_EDITOR.md §2). To upgrade, change REF and SHA512, and drop the
# patch once its pull request is in.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO paceholder/nodeeditor
    REF 7bbcd3e08c8330e9a5b47dbb730955849b8f547c
    SHA512 93c89af306b106d549b9a1027070049905195d717c98cc13226dfbc41654c8325ce3df95f327c8c5e25b8ac9216005e1d8910b97f316857244cbbaa4ca09cc4d
    HEAD_REF master
    PATCHES
        # Upstream pull request #533, still open: moving a node never reaches the
        # model without it (issues #525 and #526).
        node-moves-reach-the-model.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DUSE_QT6=ON
        -DBUILD_TESTING=OFF
        -DBUILD_EXAMPLES=OFF
        -DBUILD_DOCS=OFF
        -DQT_NODES_DEVELOPER_DEFAULTS=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/QtNodes)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.rst")
