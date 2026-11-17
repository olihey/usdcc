# The webdav:// protocol of remote assets (docs/WEBDAV_RESOLVER.md).
usdcc_add_asset_protocol(usdccWebDavProtocol
    ID org.usdcc.webdav-assets
    NAME "WebDAV Assets"
    VERSION 1.0.0
    DESCRIPTION "Loads USD assets from WebDAV servers through webdav:// paths, with named servers and environments."
    RESOLVER UsdccWebDavResolver
    SCHEMES webdav
    SOURCES webdav_protocol.cpp
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/plugins/webdav")
# The XML that PROPFIND replies come in.
target_link_libraries(usdccWebDavProtocol PRIVATE pugixml::pugixml)
