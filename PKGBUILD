pkgname=tether-git
pkgver=r1.1234
pkgrel=1
pkgdesc="A Wayland-native local file transfer and clipboard sync tool over mDNS"
arch=('x86_64')
url="https://github.com/zackb/tether"
license=('MIT')
depends=('gtk3' 'libnotify' 'openssl' 'wayland' 'avahi')
makedepends=('cmake' 'ninja' 'git')
provides=('tether' 'tether-bin')
conflicts=('tether' 'tether-bin')
source=("git+https://github.com/zackb/tether.git")
sha256sums=('SKIP')

pkgver() {
    cd tether
    printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
    cmake -B build -S tether -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
}

package() {
    DESTDIR="$pkgdir" cmake --install build
}
