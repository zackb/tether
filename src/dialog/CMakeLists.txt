find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(LAYER_SHELL REQUIRED gtk-layer-shell-0)

add_executable(tether-dialog
    main.cpp
)

target_include_directories(tether-dialog PRIVATE
    ${GTK3_INCLUDE_DIRS}
    ${LAYER_SHELL_INCLUDE_DIRS}
)

target_link_libraries(tether-dialog PRIVATE
    ${GTK3_LIBRARIES}
    ${LAYER_SHELL_LIBRARIES}
)

target_compile_options(tether-dialog PRIVATE
    ${GTK3_CFLAGS_OTHER}
    ${LAYER_SHELL_CFLAGS_OTHER}
)

# Output into root build dir
set_target_properties(tether-dialog PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

install(TARGETS tether-dialog
        RUNTIME DESTINATION bin)
