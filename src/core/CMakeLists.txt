add_library(tether_core STATIC
    src/core.cpp
    src/event_loop.cpp
    src/net.cpp
    src/clipboard.cpp
    src/base64.cpp
    src/file_transfer.cpp
    src/wayland.cpp
    src/wayland_protocols/wayland.cpp
    src/wayland_protocols/wlr-data-control-unstable-v1.cpp
)

target_include_directories(tether_core PUBLIC
    include
)

target_link_libraries(tether_core PUBLIC 
    nlohmann_json::nlohmann_json
    PkgConfig::WAYLAND
)
