#!/bin/sh

# Create directory structure
echo Create directory structure
mkdir sdrpp_debian_amd64
mkdir sdrpp_debian_amd64/DEBIAN

# Create package info
echo Create package info
# Z2 SDR: own package name. It installs the same files as SDR++ (/usr/bin/sdrpp, ...),
# so it replaces an installed official sdrpp package instead of silently upgrading it.
APP_VER=$(sed -n 's/^#define APP_VERSION "\(.*\)"/\1/p' core/src/version.h)
echo Package: z2sdr >> sdrpp_debian_amd64/DEBIAN/control
echo Version: ${APP_VER:-0.0.0}$BUILD_NO >> sdrpp_debian_amd64/DEBIAN/control
echo 'Maintainer: Zoltan Doczi <zoltan.doczi@gmail.com>' >> sdrpp_debian_amd64/DEBIAN/control
echo Architecture: all >> sdrpp_debian_amd64/DEBIAN/control
echo Conflicts: sdrpp >> sdrpp_debian_amd64/DEBIAN/control
echo Replaces: sdrpp >> sdrpp_debian_amd64/DEBIAN/control
echo 'Description: Z2 SDR, SDR receiver based on SDR++ with ESP32-S3 (ESP-SDR) support' >> sdrpp_debian_amd64/DEBIAN/control
echo Depends: $2 >> sdrpp_debian_amd64/DEBIAN/control

# Copying files
ORIG_DIR=$PWD
cd $1
make install DESTDIR=$ORIG_DIR/sdrpp_debian_amd64
cd $ORIG_DIR

# Create package
echo Create package
dpkg-deb --build sdrpp_debian_amd64

# Cleanup
echo Cleanup
rm -rf sdrpp_debian_amd64
