cmake_minimum_required(VERSION 3.19)
project(tether VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
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

# Wrapper script for native host
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/extension/host/tether-native-host.in"
    "${CMAKE_CURRENT_BINARY_DIR}/tether-native-host"
    @ONLY
)

install(PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/tether-native-host"
        DESTINATION ${CMAKE_INSTALL_BINDIR})

# native messaging manifests
set(CHROME_MESSAGING_DIR "/etc/chromium/native-messaging-hosts" CACHE PATH "Chrome native messaging hosts directory")
set(GOOGLE_CHROME_MESSAGING_DIR "/etc/opt/chrome/native-messaging-hosts" CACHE PATH "Google Chrome native messaging hosts directory")
set(MOZILLA_MESSAGING_DIR "${CMAKE_INSTALL_LIBDIR}/mozilla/native-messaging-hosts" CACHE PATH "Mozilla native messaging hosts directory")
set(THUNDERBIRD_MESSAGING_DIR "${CMAKE_INSTALL_LIBDIR}/thunderbird/native-messaging-hosts" CACHE PATH "Thunderbird native messaging hosts directory")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/com.tether.extension.chrome.json"
        DESTINATION "${CHROME_MESSAGING_DIR}"
        RENAME "com.tether.extension.json")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/com.tether.extension.chrome.json"
        DESTINATION "${GOOGLE_CHROME_MESSAGING_DIR}"
        RENAME "com.tether.extension.json")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/com.tether.extension.mozilla.json"
        DESTINATION "${MOZILLA_MESSAGING_DIR}"
        RENAME "com.tether.extension.json")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/com.tether.extension.mozilla.json"
        DESTINATION "${THUNDERBIRD_MESSAGING_DIR}"
        RENAME "com.tether.extension.json")

# install man pages
install(FILES docs/man/man1/tether.1 docs/man/man1/tether-gtk.1
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES docs/man/man8/tetherd.8
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man8)

# Extension Building only if npm is available
option(TETHER_BUILD_EXTENSIONS "Build browser and mail extensions automatically" ON)
find_program(NPM_EXECUTABLE npm)

if(TETHER_BUILD_EXTENSIONS AND NPM_EXECUTABLE)
    set(EXTENSION_BUILD_DIR "${CMAKE_BINARY_DIR}/extension_bundles")

    add_custom_target(extensions ALL
        COMMAND ${CMAKE_COMMAND} -E env BUILD_DIR=${EXTENSION_BUILD_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/extension/build.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Building browser and mail extensions"
        VERBATIM
    )

    # Install extensions into the package
    install(DIRECTORY "${EXTENSION_BUILD_DIR}/"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/tether/extensions"
            OPTIONAL
            FILES_MATCHING PATTERN "*.zip" PATTERN "*.xpi")
endif()

# CPack Configuration
set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGE_RELOCATABLE FALSE)
set(CPACK_PACKAGE_NAME "tether")
set(CPACK_PACKAGE_VENDOR "zackb")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A Wayland-native local file transfer and clipboard sync tool over mDNS")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "zack@bartel.com")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "tether")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

# DEB
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "zack@bartel.com")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgtk-3-0, libnotify4, openssl, libwayland-client0, libavahi-client3")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/zackb/tether")

# RPM
set(CPACK_RPM_PACKAGE_REQUIRES "gtk3, libnotify, openssl, wayland-client, avahi-client")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_RELOCATABLE FALSE)

# Generators
set(CPACK_GENERATOR "TGZ;DEB;RPM")

include(CPack)
