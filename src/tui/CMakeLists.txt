find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSES REQUIRED IMPORTED_TARGET ncursesw)

add_library(tether_tui_util STATIC util.cpp)
target_include_directories(tether_tui_util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tether_tui_util PUBLIC tether)

add_executable(tether-tui
    main.cpp
    calls.cpp
    contacts.cpp
    devices.cpp
    messages.cpp
    notifications.cpp
    settings.cpp
)

target_compile_definitions(tether-tui PRIVATE NCURSES_WIDECHAR=1)
target_link_libraries(tether-tui PRIVATE tether tether_ui_common tether_tui_util PkgConfig::NCURSES)

set_target_properties(tether-tui PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

include(GNUInstallDirs)
install(TARGETS tether-tui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
