add_executable(tether-gtk
    main.cpp
)

target_link_libraries(tether-gtk PRIVATE tether)

# Output into root build dir instead of nested build/src/gtk
set_target_properties(tether-gtk PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
