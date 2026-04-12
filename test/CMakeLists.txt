add_executable(tether_test
    test_base64.cpp
    test_crypto.cpp
    test_file_transfer.cpp
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
