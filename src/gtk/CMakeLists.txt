find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

add_executable(tether-gtk
    main.cpp
)

target_include_directories(tether-gtk PRIVATE ${GTK3_INCLUDE_DIRS})
target_link_libraries(tether-gtk PRIVATE tether ${GTK3_LIBRARIES})
target_compile_options(tether-gtk PRIVATE ${GTK3_CFLAGS_OTHER})

# Output into root build dir instead of nested build/src/gtk
set_target_properties(tether-gtk PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

include(GNUInstallDirs)

install(TARGETS tether-gtk
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(tether-gtk.desktop.in tether-gtk.desktop @ONLY)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/tether-gtk.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

install(FILES tether.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/1024x1024/apps)
