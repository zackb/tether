# Built by COPR through .copr/Makefile, which fills in the @...@ fields.
# The COPR project allows network access: FetchContent and npm download at build time.

Name:           tether
Version:        @VERSION@
Release:        1%{?dist}
Summary:        Bridge an iPhone to the Linux desktop: clipboard, files, messages, and notifications
License:        MIT
URL:            https://github.com/zackb/tether
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  git
BuildRequires:  pkgconf-pkg-config
BuildRequires:  wayland-devel
BuildRequires:  avahi-devel
BuildRequires:  openssl-devel
BuildRequires:  glib2-devel
BuildRequires:  libsecret-devel
BuildRequires:  gtk3-devel
BuildRequires:  gtk-layer-shell-devel
BuildRequires:  libnotify-devel
BuildRequires:  ncurses-devel
BuildRequires:  libavcodec-free-devel
BuildRequires:  pipewire-devel
BuildRequires:  gettext
BuildRequires:  npm
BuildRequires:  zip

Requires:       bluez
Requires:       bluez-obexd

%description
Tether bridges an iPhone to the Linux desktop: clipboard sync, file transfer,
messages, calls, and notifications.

%prep
%autosetup

%build
%cmake -DTETHER_VERSION=@TETHER_VERSION@
%cmake_build

%install
%cmake_install
%find_lang %{name}

%check
%ctest

%files -f %{name}.lang
%license LICENSE
%doc README.md
%{_bindir}/tether
%{_bindir}/tether-dialog
%{_bindir}/tether-gtk
%{_bindir}/tether-native-host
%{_bindir}/tether-tui
%{_bindir}/tetherd
%{_sysconfdir}/chromium/native-messaging-hosts/com.tether.extension.json
%{_sysconfdir}/opt/chrome/native-messaging-hosts/com.tether.extension.json
%{_sysconfdir}/ufw/applications.d/tether
%{_libdir}/mozilla/native-messaging-hosts/com.tether.extension.json
%{_libdir}/thunderbird/native-messaging-hosts/com.tether.extension.json
%{_prefix}/lib/firewalld/services/tether.xml
%{_unitdir}/tether-btclass@.service
%{_userunitdir}/tetherd.service
%{_datadir}/applications/tether-gtk.desktop
%{_datadir}/icons/hicolor/*/apps/tether*
%{_datadir}/kio/servicemenus/tether-send.desktop
%{_metainfodir}/io.github.zackb.tether.metainfo.xml
%{_datadir}/tether/
%{_mandir}/man1/tether.1*
%{_mandir}/man1/tether-gtk.1*
%{_mandir}/man8/tetherd.8*
