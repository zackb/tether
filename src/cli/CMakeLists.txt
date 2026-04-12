add_executable(tether_cli
    main.cpp
)

target_link_libraries(tether_cli PRIVATE tether)

# Output into root build dir instead of nested build/src/cli
set_target_properties(tether_cli PROPERTIES 
    OUTPUT_NAME tether
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
