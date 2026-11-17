# usdcc.peers — rooms of usdcc instances, programs and browsers that share data over
# WebSocket (docs/PEERS.md). Phase 1a: hosting a room and joining one by address,
# the frame, channels with their three kinds of delivery, who may send, limits, and
# guests with the page the room serves them.
#
# SHARED, for the plugins that use it (the viewport broadcast first) and later the
# Python module. Qt Core, Network, WebSockets and HttpServer only: no USD, no
# widgets, and no usdcc_core, so a program can link it alone.
add_library(usdcc_peers SHARED
    include/usdcc/peers/channel.h
    include/usdcc/peers/export.h
    include/usdcc/peers/frame.h
    include/usdcc/peers/room.h
    include/usdcc/peers/types.h
    src/channel.cpp
    src/frame.cpp
    src/guest_page.cpp
    src/log.h
    src/page_server.cpp
    src/page_server.h
    src/room.cpp
    src/room_private.h
    src/send_queue.cpp
    src/send_queue.h
    src/types.cpp
)
target_include_directories(usdcc_peers PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(usdcc_peers PRIVATE USDCC_PEERS_BUILD)
target_link_libraries(usdcc_peers PUBLIC Qt6::Core Qt6::Network Qt6::WebSockets PRIVATE Qt6::HttpServer)
set_target_properties(usdcc_peers PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
