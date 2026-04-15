add_executable(tetherd
    main.cpp
    notification.cpp
)

pkg_check_modules(LIBNOTIFY REQUIRED IMPORTED_TARGET libnotify)

target_link_libraries(tetherd PRIVATE tether PkgConfig::LIBNOTIFY)

# Output into root build dir instead of nested build/src/daemon
set_target_properties(tetherd PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

install(TARGETS tetherd
        RUNTIME DESTINATION bin)
