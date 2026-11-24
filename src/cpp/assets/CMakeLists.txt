# usdcc.assets — loading USD assets from servers (docs/REMOTE_ASSETS.md): the
# configuration and environments, stacks, the resolver context, the disk cache,
# credentials, and CachingResolver, the ArResolver base class a protocol plugin
# derives from.
#
# SHARED: the transport registry, the stack registry and the credential provider
# must exist once in the process, for the app and for every protocol plugin.
# No Qt and no usdcc_core, so that usdcat and usdview can load a protocol plugin
# with only this library beside it.
add_library(usdcc_assets SHARED
    include/usdcc/assets/activity.h
    include/usdcc/assets/caching_resolver.h
    include/usdcc/assets/config.h
    include/usdcc/assets/config_editor.h
    include/usdcc/assets/context.h
    include/usdcc/assets/credentials.h
    include/usdcc/assets/disk_cache.h
    include/usdcc/assets/http_client.h
    include/usdcc/assets/lookup.h
    include/usdcc/assets/paths.h
    include/usdcc/assets/result.h
    include/usdcc/assets/sources.h
    include/usdcc/assets/transport.h
    include/usdcc/assets/uri.h
    src/activity.cpp
    src/caching_resolver.cpp
    src/config.cpp
    src/config_editor.cpp
    src/context.cpp
    src/credentials.cpp
    src/disk_cache.cpp
    src/http_client.cpp
    src/logins.cpp
    src/lookup.cpp
    src/sources.cpp
    src/transport.cpp
    src/uri.cpp
)
target_include_directories(usdcc_assets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" ${PXR_INCLUDE_DIRS})
# Only the OpenUSD libraries it uses, not ${PXR_LIBRARIES}: a tool that loads a
# protocol plugin shouldn't have Hydra pulled in with it.
target_link_libraries(usdcc_assets PUBLIC ar plug js vt tf arch)
# HTTP, for the protocols that speak it (http_client.h). Private: a protocol plugin
# goes through that header and never sees libcurl.
target_link_libraries(usdcc_assets PRIVATE CURL::libcurl)
if(WIN32)
    # Windows Credential Manager (credentials.h).
    target_link_libraries(usdcc_assets PRIVATE advapi32)
endif()
usdcc_export_library(usdcc_assets)
