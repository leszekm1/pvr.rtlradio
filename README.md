# Realtek RTL2832U RTL-SDR Radio PVR Client



Concept based on [__pvr.rtl.radiofm__](https://github.com/AlwinEsch/pvr.rtl.radiofm) - Copyright (C)2015-2018 Alwin Esch   
FM Radio Digital Signal Processing derived from [__CuteSDR__](https://sourceforge.net/projects/cutesdr/) - Copyright (C)2010 Moe Wheatley   
Hybrid Digital (HD) Radio Digital Signal Processing derived from [__NRSC5__](https://github.com/theori-io/nrsc5)   
Digital Audio Broadcast (DAB) Digital Signal Processing adapted from project [__welle.io__](https://github.com/AlbrechtL/welle.io)

[![Build and run tests](https://github.com/leszekm1/pvr.rtlradio/actions/workflows/build.yml/badge.svg?branch=Omega)](https://github.com/leszekm1/pvr.rtlradio/actions/workflows/build.yml)
[![Build LibreELEC release](https://github.com/leszekm1/pvr.rtlradio/actions/workflows/release.yml/badge.svg)](https://github.com/leszekm1/pvr.rtlradio/actions/workflows/release.yml)
[![Build Status](https://dev.azure.com/teamkodi/kodi-pvr/_apis/build/status/kodi-pvr.pvr.rtlradio?branchName=Omega)](https://dev.azure.com/teamkodi/kodi-pvr/_build/latest?definitionId=85&branchName=Omega)
[![Build Status](https://jenkins.kodi.tv/view/Addons/job/kodi-pvr/job/pvr.rtlradio/job/Omega/badge/icon)](https://jenkins.kodi.tv/blue/organizations/jenkins/kodi-pvr%2Fpvr.rtlradio/branches/)

## Build instructions

When building the addon you have to use the correct branch depending on which version of Kodi you're building against. 
For example, if you're building the `Omega` branch of Kodi you should checkout the `Omega` branch of this repository. 
Also make sure you follow this README from the branch in question.

### Linux

The following instructions assume you will have built Kodi already in the `kodi-build` directory 
suggested by the README.

1. `git clone https://github.com/xbmc/xbmc.git`
2. `git clone https://github.com/kodi-pvr/pvr.rtlradio.git`
3. `cd pvr.rtlradio && mkdir build && cd build`
4. `cmake -DADDONS_TO_BUILD=pvr.rtlradio -DADDON_SRC_PREFIX=../.. -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=../../xbmc/kodi-build/addons -DPACKAGE_ZIP=1 ../../xbmc/cmake/addons`
5. `make`

The addon files will be placed in `../../xbmc/kodi-build/addons` so if you build Kodi from source and run it directly 
the addon will be available as a system addon.

## LibreELEC releases

GitHub releases provide a Kodi-installable ZIP for LibreELEC 12.2/12.2.1 on
Generic x86_64. The ZIP includes the add-on binary and the non-system shared
libraries it needs. Download the ZIP from the repository's Releases page and
use Kodi's **Add-ons > Install from zip file** command. Do not extract it first.

HD Radio audio requires the HDC-patched FAAD2 decoder. The dependency metadata
and patch are kept under `depends/common/faad2-hdc`; the LibreELEC release
workflow applies the same patch to its `faad2` package before building the
add-on.

Every version tag matching the generated add-on version, for example
`v21.1.0.1`, builds the add-on from source with the LibreELEC 12.2.1 toolchain,
validates the archive layout, uploads a SHA-256 checksum, and publishes the
GitHub release. A manual workflow run builds the same files as downloadable
workflow artifacts without publishing a release.

##### Useful links

* [Kodi's PVR user support] (http://forum.kodi.tv/forumdisplay.php?fid=167)
* [Kodi's PVR development support] (http://forum.kodi.tv/forumdisplay.php?fid=136)
