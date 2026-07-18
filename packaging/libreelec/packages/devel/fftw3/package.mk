# SPDX-License-Identifier: MIT

PKG_NAME="fftw3"
PKG_VERSION="3.3.10"
PKG_SHA256="56c932549852cddcfafdab3820b0200c7742675be92179e59e6215b340e26467"
PKG_LICENSE="GPL"
PKG_SITE="https://www.fftw.org/"
PKG_URL="https://www.fftw.org/fftw-${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain"
PKG_LONGDESC="FFTW is a C subroutine library for computing the discrete Fourier transform."
PKG_TOOLCHAIN="autotools"

PKG_CONFIGURE_OPTS_TARGET="--enable-shared \
                           --disable-static \
                           --disable-doc"

post_makeinstall_target() {
  cd "${PKG_BUILD}"

  make distclean || true

  ./configure \
    --host=${TARGET_NAME} \
    --build=${HOST_NAME} \
    --prefix=/usr \
    --enable-shared \
    --disable-static \
    --disable-doc \
    --enable-long-double

  make
  make DESTDIR="${PKG_INSTALL}" install

  make distclean || true

  ./configure \
    --host=${TARGET_NAME} \
    --build=${HOST_NAME} \
    --prefix=/usr \
    --enable-shared \
    --disable-static \
    --disable-doc \
    --enable-single

  make
  make DESTDIR="${PKG_INSTALL}" install
}
