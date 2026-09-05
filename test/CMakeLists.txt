add_executable(tether_test
    test_base64.cpp
    test_bluetooth_objects.cpp
    test_bluetooth_monitor.cpp
    test_bluetooth_pairing.cpp
    test_bluetooth_supervisor.cpp
    test_bmessage.cpp
    test_bmessage_build.cpp
    test_ancs_parser.cpp
    test_ancs_sequencer.cpp
    test_ancs_notifications.cpp
    test_groups.cpp
    test_bt_diagnostics.cpp
    test_telephony.cpp
    test_vcard.cpp
    test_journal.cpp
    test_crypto.cpp
    test_secret_store.cpp
    secret_test_env.cpp
    test_discovery.cpp
    test_event_loop.cpp
    test_display_width.cpp
    test_extension_host.cpp
    test_file_transfer.cpp
    test_log.cpp
    test_net.cpp
    test_otp.cpp
    test_paths.cpp
    test_message_format.cpp
    ../src/gtk/message_format.cpp
)

target_include_directories(tether_test PRIVATE
    ../src/core/include
)

target_link_libraries(tether_test PRIVATE
    tether
    GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(tether_test)

# The same gates the i18n workflow runs, so a missing translation fails here first.
find_program(MSGFMT_EXECUTABLE msgfmt)
if(MSGFMT_EXECUTABLE)
    add_test(NAME po_catalogs COMMAND "${CMAKE_SOURCE_DIR}/po/check-catalogs.sh")
endif()

find_program(XGETTEXT_EXECUTABLE xgettext)
if(XGETTEXT_EXECUTABLE)
    add_test(NAME po_template COMMAND "${CMAKE_SOURCE_DIR}/po/update-pot.sh" --check)
endif()
