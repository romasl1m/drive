#!/bin/bash
set -e

VERSION="${1:-1.0.0}"
ARCH="amd64"
PKG_NAME="drive"
PKG_DIR="$(mktemp -d)"
SRC_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "Building Drive ${VERSION} .deb package..."

# Build the binary
echo "  Compiling..."
BUILD_DIR="${SRC_DIR}/build_app"
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"
cmake "${SRC_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${SRC_DIR}/build" \
    >/dev/null 2>&1
make -j"$(nproc)" 2>&1 | tail -3
strip drive

# Assemble the package
echo "  Assembling package..."
mkdir -p "${PKG_DIR}/DEBIAN"
mkdir -p "${PKG_DIR}/usr/bin"
mkdir -p "${PKG_DIR}/usr/share/applications"
mkdir -p "${PKG_DIR}/usr/share/icons/hicolor/scalable/apps"
mkdir -p "${PKG_DIR}/etc/xdg/autostart"

# Binary
cp "${BUILD_DIR}/drive" "${PKG_DIR}/usr/bin/drive"
chmod 755 "${PKG_DIR}/usr/bin/drive"

# Desktop entry (app launcher)
cp "${SRC_DIR}/resources/drive.desktop" "${PKG_DIR}/usr/share/applications/drive.desktop"

# Autostart entry (background backup on login)
cp "${SRC_DIR}/resources/drive-autostart.desktop" "${PKG_DIR}/etc/xdg/autostart/drive.desktop"

# Icon
cp "${SRC_DIR}/resources/drive.svg" "${PKG_DIR}/usr/share/icons/hicolor/scalable/apps/drive.svg"

# Maintainer scripts
cp "${SRC_DIR}/pkg/postinst" "${PKG_DIR}/DEBIAN/postinst"
cp "${SRC_DIR}/pkg/prerm" "${PKG_DIR}/DEBIAN/prerm"
cp "${SRC_DIR}/pkg/postrm" "${PKG_DIR}/DEBIAN/postrm"
chmod 755 "${PKG_DIR}/DEBIAN/postinst"
chmod 755 "${PKG_DIR}/DEBIAN/prerm"
chmod 755 "${PKG_DIR}/DEBIAN/postrm"

INSTALLED_SIZE=$(du -sk "${PKG_DIR}/usr" | cut -f1)

cat > "${PKG_DIR}/DEBIAN/control" <<EOF
Package: ${PKG_NAME}
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Installed-Size: ${INSTALLED_SIZE}
Depends: libsqlite3-0, libssl3, zlib1g, libqt5widgets5, libqt5network5, libqt5svg5
Maintainer: romasl1m
Description: Private unlimited backup to Telegram
 Drive automatically backs up your selected folders to a private
 Telegram channel. It runs in the background, syncs files instantly
 when they change, and lets you restore them at any time.
 .
 Features:
  - Unlimited storage with no file size limits
  - Automatic sync on file changes
  - Restore any file or folder
  - Private — stored in your own Telegram channel
  - No subscriptions, no third-party servers
Homepage: https://github.com/romasl1m/drive
EOF

# Build the .deb
echo "  Packaging..."
OUTPUT="${SRC_DIR}/${PKG_NAME}_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "${PKG_DIR}" "${OUTPUT}" >/dev/null 2>&1

rm -rf "${PKG_DIR}"

SIZE=$(du -h "${OUTPUT}" | cut -f1)
echo ""
echo "Done: ${OUTPUT} (${SIZE})"
echo ""
echo "Install:  sudo apt install ./${PKG_NAME}_${VERSION}_${ARCH}.deb"
echo "Launch:   Search 'Drive' in your application menu"
