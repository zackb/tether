add_library(tether STATIC
    src/core.cpp
    src/event_loop.cpp
    src/net.cpp
    src/clipboard.cpp
    src/base64.cpp
    src/otp.cpp
    src/file_transfer.cpp
    src/crypto.cpp
    src/client.cpp
    src/discovery.cpp
    src/bluetooth/objects.cpp
    src/bluetooth/monitor.cpp
    src/bluetooth/config.cpp
    src/bluetooth/agent.cpp
    src/bluetooth/advert.cpp
    src/bluetooth/pairing.cpp
    src/bluetooth/bearer_supervisor.cpp
    src/bluetooth/profile_supervisor.cpp
    src/bluetooth/vcard.cpp
    src/bluetooth/bmessage.cpp
    src/bluetooth/messages.cpp
    src/bluetooth/map_session.cpp
    src/bluetooth/contacts.cpp
    src/bluetooth/journal.cpp
    src/bluetooth/pbap_session.cpp
    src/bluetooth/obex_transfer.cpp
    src/bluetooth/ancs/parser.cpp
    src/bluetooth/ancs/notifications.cpp
    src/bluetooth/ancs/sequencer.cpp
    src/bluetooth/ancs/client.cpp
    src/bluetooth/groups.cpp
    src/bluetooth/connection.cpp
    src/bluetooth/diagnostics.cpp
    src/wayland.cpp
    src/wayland_protocols/wayland.cpp
    src/wayland_protocols/wlr-data-control-unstable-v1.cpp
)

pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0)

target_include_directories(tether PUBLIC
    include
    ${CMAKE_BINARY_DIR}/generated
)


target_link_libraries(tether PUBLIC
    nlohmann_json::nlohmann_json
    PkgConfig::WAYLAND
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::AVAHI
    PkgConfig::GLIB
)
