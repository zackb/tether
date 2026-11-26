pkgname=tether-git
pkgver=0.3.7
pkgrel=1
pkgdesc="Bridge an iPhone to the Linux desktop: clipboard, files, messages, and notifications"
arch=('x86_64' 'aarch64')
url="https://github.com/zackb/tether"
license=('MIT')
depends=('gtk3' 'gtk-layer-shell' 'libnotify' 'ncurses' 'openssl' 'wayland' 'avahi' 'glib2' 'libsecret' 'bluez' 'bluez-utils' 'bluez-obex' 'ffmpeg' 'pipewire')
makedepends=('cmake' 'ninja' 'git')
provides=('tether')
conflicts=('tether' 'tether-bin')
install=packaging/tether.install
source=("git+${url}.git")
sha256sums=('SKIP')

pkgver() {
    cd "${pkgname%-git}"
    git describe --long --tags | sed 's/\([^-]*-g\)/r\1/;s/-/./g;s/^v//'
}

build() {
    cmake -B build -S "${pkgname%-git}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DTETHER_BUILD_EXTENSIONS=OFF
    cmake --build build
}

package() {
    DESTDIR="$pkgdir" cmake --install build
}
