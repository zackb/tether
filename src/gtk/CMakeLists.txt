find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

add_executable(tether-gtk
    main.cpp
    ui_util.cpp
    prefs.cpp
    calls_view.cpp
    contact_completion.cpp
    contacts_view.cpp
    daemon_client.cpp
    devices_view.cpp
    messages_view.cpp
    notifications_view.cpp
    settings_view.cpp
    tray.cpp
)

target_include_directories(tether-gtk PRIVATE ${GTK3_INCLUDE_DIRS})
target_link_libraries(tether-gtk PRIVATE tether tether_ui_common ${GTK3_LIBRARIES})
target_compile_options(tether-gtk PRIVATE ${GTK3_CFLAGS_OTHER})

# Output into root build dir instead of nested build/src/gtk
set_target_properties(tether-gtk PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

include(GNUInstallDirs)

install(TARGETS tether-gtk
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# msgfmt --desktop injects Name[xx]/GenericName[xx]/Comment[xx] from the catalogs.
# Without gettext tools the entry is still installed, English only.
find_program(MSGFMT_EXECUTABLE msgfmt)
file(GLOB TETHER_PO_FILES "${CMAKE_SOURCE_DIR}/po/*.po")
if(MSGFMT_EXECUTABLE AND TETHER_PO_FILES)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/tether-gtk.desktop"
        COMMAND ${MSGFMT_EXECUTABLE} --desktop
                --template "${CMAKE_CURRENT_SOURCE_DIR}/tether-gtk.desktop.in"
                -d "${CMAKE_SOURCE_DIR}/po"
                -o "${CMAKE_CURRENT_BINARY_DIR}/tether-gtk.desktop"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tether-gtk.desktop.in" ${TETHER_PO_FILES}
        COMMENT "Localizing tether-gtk.desktop"
        VERBATIM)
    add_custom_target(desktop-file ALL
                      DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/tether-gtk.desktop")
else()
    configure_file(tether-gtk.desktop.in tether-gtk.desktop @ONLY)
endif()

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/tether-gtk.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

# Software centers (GNOME Software, Discover, Flathub) read this listing.
install(FILES com.tether.desktop.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

foreach(SIZE 16 32 48 64 128 256 512 1024)
    install(FILES icons/${SIZE}x${SIZE}/tether.png
                  icons/${SIZE}x${SIZE}/tether-offline.png
                  icons/${SIZE}x${SIZE}/tether-unread.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${SIZE}x${SIZE}/apps)
endforeach()

install(FILES icons/symbolic/tether-symbolic.svg
              icons/symbolic/tether-offline-symbolic.svg
              icons/symbolic/tether-unread-symbolic.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/symbolic/apps)
