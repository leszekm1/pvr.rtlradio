# SPDX-License-Identifier: MIT

PKG_NAME="rtl-sdr"
PKG_VERSION="2.0.2"
PKG_SHA256="d69943eb32df742bc38a00ce6615e41250fd57851174e5ff916ec31e9e9e68e9"
PKG_LICENSE="GPL-2.0"
PKG_SITE="https://osmocom.org/projects/rtl-sdr/wiki"
PKG_URL="https://github.com/osmocom/rtl-sdr/archive/refs/tags/v${PKG_VERSION}.tar.gz"
PKG_DEPENDS_TARGET="toolchain libusb"
PKG_LONGDESC="Software to turn RTL2832U-based DVB-T receivers into SDR receivers."
PKG_TOOLCHAIN="cmake"
PKG_BUILD_FLAGS="+pic"

PKG_CMAKE_OPTS_TARGET="-DINSTALL_UDEV_RULES=OFF -DDETACH_KERNEL_DRIVER=ON"
