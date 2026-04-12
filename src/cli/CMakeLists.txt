add_executable(tether
    main.cpp
)

target_link_libraries(tether PRIVATE tether_core)

# Output into root build dir instead of nested build/src/cli
set_target_properties(tether PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
