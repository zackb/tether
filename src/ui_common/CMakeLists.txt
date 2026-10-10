add_library(tether_ui_common STATIC
    message_format.cpp
    ui_text.cpp
)

target_include_directories(tether_ui_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tether_ui_common PUBLIC tether)
