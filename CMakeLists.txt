cmake_minimum_required(VERSION 3.19)
project(tether VERSION 0.2.30 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# version from git unless a packager supplied one
if(NOT DEFINED TETHER_VERSION)
    execute_process(
        COMMAND git describe --tags --always --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(GIT_VERSION)
        string(REGEX REPLACE "^v" "" TETHER_VERSION "${GIT_VERSION}")
    else()
        set(TETHER_VERSION "${PROJECT_VERSION}-unknown")
    endif()
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/include/tether/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/tether/version.hpp"
    @ONLY
)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)
find_package(OpenSSL REQUIRED)
find_package(Intl REQUIRED)
pkg_check_modules(SECRET REQUIRED IMPORTED_TARGET libsecret-1)

include(FetchContent)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW ON
)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  GIT_SHALLOW ON
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)

enable_testing()

# subdirectories for the targets
add_subdirectory(src)
add_subdirectory(po)
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

# systemd user unit for the daemon. The distro packages install it with the real
# path in ExecStart; a portable build carries a copy with a placeholder the CLI
# fills in with wherever that build happens to live.
set(TETHERD_EXEC "${CMAKE_INSTALL_FULL_BINDIR}/tetherd")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/tetherd.service.in"
    "${CMAKE_CURRENT_BINARY_DIR}/tetherd.service"
    @ONLY
)
set(TETHERD_EXEC "__TETHERD_EXEC__")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/tetherd.service.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/tetherd.service.tmpl"
    @ONLY
)

# embed files only the distro packages can install, so portable build can write them out at runtime.
file(READ "${CMAKE_CURRENT_BINARY_DIR}/generated/tetherd.service.tmpl" TETHERD_UNIT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/packaging/systemd/tether-btclass@.service" BTCLASS_UNIT)
file(READ "${CMAKE_CURRENT_BINARY_DIR}/com.tether.extension.chrome.json" CHROME_MANIFEST)
file(READ "${CMAKE_CURRENT_BINARY_DIR}/com.tether.extension.mozilla.json" MOZILLA_MANIFEST)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/include/tether/packaging.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/tether/packaging.hpp"
    @ONLY
)

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

string(TIMESTAMP BUILD_DATE "%B %Y")

configure_file(docs/man/man1/tether.1.in "${CMAKE_CURRENT_BINARY_DIR}/man/tether.1" @ONLY)
configure_file(docs/man/man1/tether-gtk.1.in "${CMAKE_CURRENT_BINARY_DIR}/man/tether-gtk.1" @ONLY)
configure_file(docs/man/man8/tetherd.8.in "${CMAKE_CURRENT_BINARY_DIR}/man/tetherd.8" @ONLY)

# install man pages
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/man/tether.1" "${CMAKE_CURRENT_BINARY_DIR}/man/tether-gtk.1"
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/man/tetherd.8"
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man8)

set(SYSTEMD_UNIT_DIR "lib/systemd/system" CACHE PATH "systemd system unit directory")
install(FILES packaging/systemd/tether-btclass@.service
        DESTINATION "${SYSTEMD_UNIT_DIR}")

set(SYSTEMD_USER_UNIT_DIR "lib/systemd/user" CACHE PATH "systemd user unit directory")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/tetherd.service"
        DESTINATION "${SYSTEMD_USER_UNIT_DIR}")

# firewall profiles for the mTLS listener (5134/tcp) and mDNS (5353/udp) installed but not applied (user's call).
set(UFW_APPLICATIONS_DIR "/etc/ufw/applications.d" CACHE PATH "ufw application profile directory")
set(FIREWALLD_SERVICE_DIR "lib/firewalld/services" CACHE PATH "firewalld service directory")
# write them only where the directory already exists
function(install_if_dir_exists src dir name)
    install(CODE "
        set(_dir \"${dir}\")
        if(NOT IS_ABSOLUTE \"\${_dir}\")
            set(_dir \"\${CMAKE_INSTALL_PREFIX}/\${_dir}\")
        endif()
        if(NOT \"\$ENV{DESTDIR}\" STREQUAL \"\" OR IS_DIRECTORY \"\${_dir}\")
            file(INSTALL DESTINATION \"\${_dir}\" TYPE FILE RENAME \"${name}\" FILES \"${src}\")
        else()
            message(STATUS \"Skipping \${_dir}/${name}: directory not present\")
        endif()
    ")
endfunction()

install_if_dir_exists("${CMAKE_CURRENT_SOURCE_DIR}/packaging/firewall/tether.ufw"
                      "${UFW_APPLICATIONS_DIR}" tether)
install_if_dir_exists("${CMAKE_CURRENT_SOURCE_DIR}/packaging/firewall/tether.xml"
                      "${FIREWALLD_SERVICE_DIR}" tether.xml)


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

    # Install only the finalized extension bundles into the package
    install(FILES
            "${EXTENSION_BUILD_DIR}/tether-browser-extension.zip"
            "${EXTENSION_BUILD_DIR}/tether-mail-extension.xpi"
            "${EXTENSION_BUILD_DIR}/tether-chromium-extension.zip"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/tether/extensions"
            COMPONENT extensions
            OPTIONAL)
endif()

# CPack Configuration
set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGE_RELOCATABLE FALSE)
set(CPACK_PACKAGE_NAME "tether")
set(CPACK_PACKAGE_VENDOR "zackb")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Bridge an iPhone to the Linux desktop: clipboard, files, messages, and notifications")
set(CPACK_PACKAGE_VERSION "${TETHER_VERSION}")

set(CPACK_PACKAGE_CONTACT "zack@bartel.com")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "tether")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

# remove the -Linux suffix
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}")

# DEB
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "zack@bartel.com")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
# alternates cover Ubuntu 24.04's t64 renames
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgtk-3-0 | libgtk-3-0t64, libgtk-layer-shell0, libnotify4, libssl3 | libssl3t64, libwayland-client0, libavahi-client3, libglib2.0-0 | libglib2.0-0t64, bluez, bluez-obexd")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/zackb/tether")

# RPM
set(CPACK_RPM_PACKAGE_REQUIRES "gtk3, gtk-layer-shell, libnotify, openssl-libs, libwayland-client, avahi-libs, glib2, bluez, bluez-obexd")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_RELOCATABLE FALSE)

# Generators
set(CPACK_GENERATOR "TGZ;DEB;RPM")

include(CPack)
