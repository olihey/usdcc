# usdcc.peers — rooms of usdcc instances, programs and browsers that share data over
# WebSocket (docs/PEERS.md): identities, hosting and joining with an encrypted
# handshake, channels with their three kinds of delivery, shared state, transfers, reconnecting,
# guests with the page the room serves them, and finding rooms on this machine and
# network.
#
# SHARED, for the plugins that use it (the viewport broadcast first) and later the
# Python module. Qt Core, Network, WebSockets and HttpServer, libsodium for the
# encryption and mdns for finding rooms: no USD, no widgets, and no usdcc_core, so a
# program can link it alone.
add_library(usdcc_peers SHARED
    include/usdcc/peers/channel.h
    include/usdcc/peers/discovery.h
    include/usdcc/peers/export.h
    include/usdcc/peers/frame.h
    include/usdcc/peers/identity.h
    include/usdcc/peers/peers.h
    include/usdcc/peers/room.h
    include/usdcc/peers/state.h
    include/usdcc/peers/transfer.h
    include/usdcc/peers/types.h
    src/channel.cpp
    src/crypto.cpp
    src/crypto.h
    src/discovery.cpp
    src/frame.cpp
    src/identity.cpp
    src/guest_page.cpp
    src/log.h
    src/mdns_discovery.cpp
    src/page_server.cpp
    src/peers.cpp
    src/page_server.h
    src/room.cpp
    src/room_private.h
    src/send_queue.cpp
    src/send_queue.h
    src/state.cpp
    src/transfer.cpp
    src/types.cpp
)
target_include_directories(usdcc_peers PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(usdcc_peers PRIVATE USDCC_PEERS_BUILD)
target_link_libraries(usdcc_peers PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets
                                   PRIVATE Qt6::HttpServer unofficial-sodium::sodium)
# mdns: one header (src/mdns_discovery.cpp).
find_path(USDCC_MDNS_INCLUDE_DIR mdns.h REQUIRED)
target_include_directories(usdcc_peers PRIVATE "${USDCC_MDNS_INCLUDE_DIR}")
if(WIN32)
    # The profile's key in the Windows credential store (identity.cpp), and the
    # sockets mdns uses.
    target_link_libraries(usdcc_peers PRIVATE advapi32 ws2_32 iphlpapi)
endif()
set_target_properties(usdcc_peers PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
