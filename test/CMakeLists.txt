add_executable(tether_test
    test_base64.cpp
    test_bluetooth_objects.cpp
    test_bluetooth_pairing.cpp
    test_bluetooth_supervisor.cpp
    test_bmessage.cpp
    test_bmessage_build.cpp
    test_ancs_parser.cpp
    test_ancs_sequencer.cpp
    test_groups.cpp
    test_bt_diagnostics.cpp
    test_vcard.cpp
    test_journal.cpp
    test_crypto.cpp
    test_discovery.cpp
    test_file_transfer.cpp
    test_otp.cpp
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
