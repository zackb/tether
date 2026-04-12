add_executable(tetherd
    main.cpp
)

target_link_libraries(tetherd PRIVATE tether)

# Output into root build dir instead of nested build/src/daemon
set_target_properties(tetherd PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
