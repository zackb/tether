cmake_minimum_required(VERSION 3.19)
project(tether CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)
find_package(OpenSSL REQUIRED)

include(FetchContent)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG release-1.12.1
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)

enable_testing()

# subdirectories for the targets
add_subdirectory(src)
add_subdirectory(test)

# Add uninstall target
if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        IMMEDIATE @ONLY)

    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()

# Browser Extension Host manifest generation
include(GNUInstallDirs)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/extension/host/com.tether.extension.chrome.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/com.tether.extension.chrome.json"
    @ONLY
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/extension/host/com.tether.extension.mozilla.json.in"
    "${CMAKE_CURRENT_BINARY_DIR}/com.tether.extension.mozilla.json"
    @ONLY
)

install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/extension/host/tether-native-host"
        DESTINATION ${CMAKE_INSTALL_BINDIR})

