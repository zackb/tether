add_library(tether STATIC
    src/core.cpp
    src/event_loop.cpp
    src/net.cpp
    src/clipboard.cpp
    src/base64.cpp
    src/file_transfer.cpp
    src/crypto.cpp
    src/client.cpp
    src/discovery.cpp
    src/wayland.cpp
    src/wayland_protocols/wayland.cpp
    src/wayland_protocols/wlr-data-control-unstable-v1.cpp
)

pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0)

target_include_directories(tether PUBLIC
    include
)

target_link_libraries(tether PUBLIC 
    nlohmann_json::nlohmann_json
    PkgConfig::WAYLAND
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::AVAHI
    PkgConfig::GLIB
)
