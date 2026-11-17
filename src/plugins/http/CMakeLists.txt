# The plain http:// and https:// protocol of remote assets (docs/HTTP_RESOLVER.md).
usdcc_add_asset_protocol(usdccHttpProtocol
    ID org.usdcc.http-assets
    NAME "HTTP Assets"
    VERSION 1.0.0
    DESCRIPTION "Loads USD assets from http:// and https:// URLs, read-only, through the asset cache."
    RESOLVER UsdccHttpResolver
    SCHEMES http https
    SOURCES http_protocol.cpp
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/plugins/http")
