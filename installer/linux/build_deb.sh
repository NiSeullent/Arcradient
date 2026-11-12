#!/usr/bin/env bash
# =============================================================================
# Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
# installer/linux/build_deb.sh - Debian / Ubuntu (.deb) Package Builder
#
# Usage:
#   bash installer/linux/build_deb.sh <build_dir> <output_deb_path>
# =============================================================================
set -euo pipefail

if [[ $# -eq 1 && ( "$1" == "--help" || "$1" == "-h" ) ]]; then
    echo "Usage: bash installer/linux/build_deb.sh <build_dir> <output_deb_path>"
    exit 0
fi

if [[ $# -ne 2 ]]; then
    echo "Usage: bash installer/linux/build_deb.sh <build_dir> <output_deb_path>" >&2
    exit 1
fi

BUILD_DIR="$1"
OUTPUT_DEB="$2"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "build_deb.sh: build directory '${BUILD_DIR}' does not exist" >&2
    exit 1
fi

# Locate main browser executable in <build_dir>
MAIN_BIN=""
if [[ -f "${BUILD_DIR}/waterscape" ]]; then
    MAIN_BIN="${BUILD_DIR}/waterscape"
elif [[ -f "${BUILD_DIR}/arcradient" ]]; then
    MAIN_BIN="${BUILD_DIR}/arcradient"
else
    echo "build_deb.sh: neither 'waterscape' nor 'arcradient' binary found in '${BUILD_DIR}'" >&2
    exit 1
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/waterscape-deb-XXXXXX")"
cleanup() {
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

STAGE_DIR="${WORK_DIR}/pkgroot"
mkdir -p \
    "${STAGE_DIR}/DEBIAN" \
    "${STAGE_DIR}/usr/bin" \
    "${STAGE_DIR}/usr/lib/waterscape/addons" \
    "${STAGE_DIR}/usr/share/applications" \
    "${STAGE_DIR}/usr/share/icons/hicolor/256x256/apps" \
    "${STAGE_DIR}/usr/share/icons/hicolor/48x48/apps" \
    "${STAGE_DIR}/usr/share/icons/hicolor/scalable/apps" \
    "${STAGE_DIR}/usr/share/doc/waterscape"

# 1. Stage main binary, dynamic Pure C11 modules, plugins, and .wsx addons
install -m 0755 "${MAIN_BIN}" "${STAGE_DIR}/usr/lib/waterscape/waterscape"

shopt -s nullglob
for so_file in "${BUILD_DIR}"/libarcr_*.so* "${BUILD_DIR}"/libws_plugin_*.so*; do
    if [[ -f "${so_file}" ]]; then
        cp -a "${so_file}" "${STAGE_DIR}/usr/lib/waterscape/"
    fi
done

for wsx_file in "${BUILD_DIR}"/addons/*.wsx "${BUILD_DIR}"/*.wsx; do
    if [[ -f "${wsx_file}" ]]; then
        install -m 0644 "${wsx_file}" "${STAGE_DIR}/usr/lib/waterscape/addons/"
    fi
done
shopt -u nullglob

# 2. Stage /usr/bin/waterscape launcher wrapper
cat > "${STAGE_DIR}/usr/bin/waterscape" <<'EOF'
#!/bin/sh
set -eu
WS_LIB="/usr/lib/waterscape"
export LD_LIBRARY_PATH="${WS_LIB}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
exec "${WS_LIB}/waterscape" --module-dir "${WS_LIB}" "$@"
EOF
chmod 0755 "${STAGE_DIR}/usr/bin/waterscape"

# 3. Locate or procedurally generate Waterscape brand icons (256, 48, SVG)
BRAND_SRC=""
for cand in "${BUILD_DIR}/brand" "${BUILD_DIR}/assets" "${BUILD_DIR}"; do
    if [[ -f "${cand}/waterscape_256.png" && -f "${cand}/waterscape_48.png" && -f "${cand}/waterscape.svg" ]]; then
        BRAND_SRC="${cand}"
        break
    fi
done

if [[ -z "${BRAND_SRC}" ]]; then
    BRAND_SRC="${WORK_DIR}/generated_brand"
    mkdir -p "${BRAND_SRC}"
    if [[ -x "${BUILD_DIR}/gen_brand_assets" ]]; then
        "${BUILD_DIR}/gen_brand_assets" "${BRAND_SRC}" >/dev/null
    else
        "${CC:-gcc}" -std=c11 -O2 -I"${REPO_ROOT}/include" \
            "${REPO_ROOT}/tools/gen_brand_assets.c" \
            "${REPO_ROOT}/base/arcr_codec.c" \
            "${REPO_ROOT}/base/arcr_str.c" \
            -lm -o "${WORK_DIR}/gen_brand_assets"
        "${WORK_DIR}/gen_brand_assets" "${BRAND_SRC}" >/dev/null
    fi
fi

install -m 0644 "${BRAND_SRC}/waterscape_256.png" \
    "${STAGE_DIR}/usr/share/icons/hicolor/256x256/apps/waterscape.png"
install -m 0644 "${BRAND_SRC}/waterscape_48.png" \
    "${STAGE_DIR}/usr/share/icons/hicolor/48x48/apps/waterscape.png"
install -m 0644 "${BRAND_SRC}/waterscape.svg" \
    "${STAGE_DIR}/usr/share/icons/hicolor/scalable/apps/waterscape.svg"

# 4. Stage Freedesktop .desktop launcher & Debian copyright metadata
install -m 0644 "${SCRIPT_DIR}/waterscape.desktop" \
    "${STAGE_DIR}/usr/share/applications/waterscape.desktop"

cat > "${STAGE_DIR}/usr/share/doc/waterscape/copyright" <<'EOF'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: waterscape
Source: https://arcradient.org

Files: *
Copyright: 2026 Arcradient Project
License: MIT
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, subject to the above copyright notice.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
EOF
chmod 0644 "${STAGE_DIR}/usr/share/doc/waterscape/copyright"

# 5. Compute Installed-Size in KiB and generate DEBIAN/control, postinst, prerm
INSTALLED_KB="$(du -sk "${STAGE_DIR}/usr" | awk '{print $1}')"
if [[ -z "${INSTALLED_KB}" || "${INSTALLED_KB}" -lt 1 ]]; then
    INSTALLED_KB=64
fi

DEB_ARCH="${DEB_ARCH:-amd64}"

cat > "${STAGE_DIR}/DEBIAN/control" <<EOF
Package: waterscape
Version: 2.0.0
Section: web
Priority: optional
Architecture: ${DEB_ARCH}
Installed-Size: ${INSTALLED_KB}
Maintainer: Arcradient Project <maintainers@arcradient.org>
Homepage: https://arcradient.org
Depends: libc6, libfreetype6, libpng16-16, libssl3 | libssl1.1, libx11-6, libxext6
Description: Waterscape 2.0 hardware-accelerated Pure C11 modern web browser
 Waterscape 2.0 is a lightweight modern 2026 web browser powered by the
 Arcradient (MSHTML-Next) Pure C11 engine with QuickJS-NG ES2026, Brotli/WebP/SVG,
 Tailwind v4/oklch/60FPS CSS animations, anti-aliased CJK typography, and multi-core SIMD tiles.
EOF
chmod 0644 "${STAGE_DIR}/DEBIAN/control"

cat > "${STAGE_DIR}/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true
    fi
fi
exit 0
EOF
chmod 0755 "${STAGE_DIR}/DEBIAN/postinst"

cat > "${STAGE_DIR}/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = "remove" ] || [ "$1" = "purge" ]; then
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q /usr/share/applications || true
    fi
fi
exit 0
EOF
chmod 0755 "${STAGE_DIR}/DEBIAN/prerm"

# Normalize file & directory permissions for lintian-clean .deb archive
chmod -R go-w "${STAGE_DIR}"
find "${STAGE_DIR}" -type d -exec chmod 0755 {} +

mkdir -p "$(dirname "${OUTPUT_DEB}")"
dpkg-deb --root-owner-group --build "${STAGE_DIR}" "${OUTPUT_DEB}" >/dev/null
echo "Built Debian package: ${OUTPUT_DEB} (${INSTALLED_KB} KiB installed)"
