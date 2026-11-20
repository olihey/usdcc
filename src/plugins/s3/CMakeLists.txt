# The s3:// protocol of remote assets (docs/S3_RESOLVER.md).
usdcc_add_asset_protocol(usdccS3Protocol
    ID org.usdcc.s3-assets
    NAME "S3 Assets"
    VERSION 1.0.0
    DESCRIPTION "Loads USD assets from Amazon S3 and S3-compatible stores through s3:// paths, with named servers and environments, or by bucket."
    RESOLVER UsdccS3Resolver
    SCHEMES s3
    SOURCES s3_protocol.cpp aws_credentials.cpp aws_credentials.h
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/plugins/s3")
# The XML that listings and errors come in, and the helpers it shares with WebDAV.
target_link_libraries(usdccS3Protocol PRIVATE pugixml::pugixml)
target_include_directories(usdccS3Protocol PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../common")
