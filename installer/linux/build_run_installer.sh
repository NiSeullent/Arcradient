#!/usr/bin/env bash
# =============================================================================
# Waterscape Browser 1.0 / Arcradient Engine (MSHTML-Next)
# installer/linux/build_run_installer.sh - Universal Self-Extracting .run Builder
#
# Usage:
#   bash installer/linux/build_run_installer.sh <build_dir> <output_run_path>
#
# Produces a single-file executable POSIX shell installer (`chmod +x`) with
# embedded SHA-256 integrity verification (`--check`), `--help`, `--info`,
# `--prefix <dir>`, `--no-xdg`, `--yes`/`-y`, and generated `uninstall.sh`.
# =============================================================================
set -euo pipefail

if [[ $# -eq 1 && ( "$1" == "--help" || "$1" == "-h" ) ]]; then
    echo "Usage: bash installer/linux/build_run_installer.sh <build_dir> <output_run_path>"
    exit 0
fi

if [[ $# -ne 2 ]]; then
    echo "Usage: bash installer/linux/build_run_installer.sh <build_dir> <output_run_path>" >&2
    exit 1
fi

BUILD_DIR="$1"
OUTPUT_RUN="$2"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ ! -d "${BUILD_DIR}" ]]; then
    echo "build_run_installer.sh: build directory '${BUILD_DIR}' does not exist" >&2
    exit 1
fi

MAIN_BIN=""
if [[ -f "${BUILD_DIR}/waterscape" ]]; then
    MAIN_BIN="${BUILD_DIR}/waterscape"
elif [[ -f "${BUILD_DIR}/arcradient" ]]; then
    MAIN_BIN="${BUILD_DIR}/arcradient"
else
    echo "build_run_installer.sh: neither 'waterscape' nor 'arcradient' found in '${BUILD_DIR}'" >&2
    exit 1
fi

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/waterscape-run-XXXXXX")"
cleanup() {
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

PAYLOAD_DIR="${WORK_DIR}/payload"
mkdir -p \
    "${PAYLOAD_DIR}/lib/waterscape/addons" \
    "${PAYLOAD_DIR}/share/applications" \
    "${PAYLOAD_DIR}/share/icons/hicolor/256x256/apps" \
    "${PAYLOAD_DIR}/share/icons/hicolor/48x48/apps" \
    "${PAYLOAD_DIR}/share/icons/hicolor/scalable/apps"

# 1. Stage main binary, dynamic Pure C11 modules, and .wsx extensions
install -m 0755 "${MAIN_BIN}" "${PAYLOAD_DIR}/lib/waterscape/waterscape"

shopt -s nullglob
for so_file in "${BUILD_DIR}"/libarcr_*.so* "${BUILD_DIR}"/libws_plugin_*.so*; do
    if [[ -f "${so_file}" ]]; then
        cp -a "${so_file}" "${PAYLOAD_DIR}/lib/waterscape/"
    fi
done

for wsx_file in "${BUILD_DIR}"/addons/*.wsx "${BUILD_DIR}"/*.wsx; do
    if [[ -f "${wsx_file}" ]]; then
        install -m 0644 "${wsx_file}" "${PAYLOAD_DIR}/lib/waterscape/addons/"
    fi
done
shopt -u nullglob

# 2. Locate or procedurally generate brand icons
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

install -m 0644 "${SCRIPT_DIR}/waterscape.desktop" \
    "${PAYLOAD_DIR}/share/applications/waterscape.desktop"
install -m 0644 "${BRAND_SRC}/waterscape_256.png" \
    "${PAYLOAD_DIR}/share/icons/hicolor/256x256/apps/waterscape.png"
install -m 0644 "${BRAND_SRC}/waterscape_48.png" \
    "${PAYLOAD_DIR}/share/icons/hicolor/48x48/apps/waterscape.png"
install -m 0644 "${BRAND_SRC}/waterscape.svg" \
    "${PAYLOAD_DIR}/share/icons/hicolor/scalable/apps/waterscape.svg"

# 3. Create compressed tar.gz payload & compute SHA-256 digest
TAR_GZ="${WORK_DIR}/waterscape_payload.tar.gz"
tar -C "${PAYLOAD_DIR}" -czf "${TAR_GZ}" .

if command -v sha256sum >/dev/null 2>&1; then
    PAYLOAD_SHA256="$(sha256sum "${TAR_GZ}" | awk '{print $1}')"
else
    PAYLOAD_SHA256="$(shasum -a 256 "${TAR_GZ}" | awk '{print $1}')"
fi
PAYLOAD_BYTES="$(wc -c < "${TAR_GZ}" | tr -d ' ')"
HOST_ARCH="$(uname -m 2>/dev/null || echo x86_64)"

mkdir -p "$(dirname "${OUTPUT_RUN}")"

# 4. Emit self-extracting POSIX shell header + embedded tar.gz archive
sed \
    -e "s|@WS_ARCH@|${HOST_ARCH}|g" \
    -e "s|@WS_SHA256@|${PAYLOAD_SHA256}|g" \
    -e "s|@WS_BYTES@|${PAYLOAD_BYTES}|g" \
    <<'EOF_RUN_TEMPLATE' > "${OUTPUT_RUN}"
#!/bin/sh
# Waterscape Web Browser 1.0.0 Self-Extracting Linux Installer
set -eu

APP_NAME="Waterscape Web Browser"
APP_VERSION="1.0.0"
APP_ARCH="@WS_ARCH@"
EXPECTED_SHA256="@WS_SHA256@"
PAYLOAD_BYTES="@WS_BYTES@"

if [ "$(id -u)" = "0" ]; then
    DEFAULT_PREFIX="/opt/waterscape"
else
    DEFAULT_PREFIX="${HOME:-/tmp}/.local/opt/waterscape"
fi

PREFIX="${DEFAULT_PREFIX}"
ASSUME_YES=0
INSTALL_XDG=1
MODE="install"

print_help() {
    cat <<EOH
${APP_NAME} ${APP_VERSION} (${APP_ARCH}) Self-Extracting Installer

Usage: $0 [options]
  --help, -h          Show this help message and exit
  --info              Display embedded archive metadata and exit
  --check             Verify embedded SHA-256 payload checksum without installing
  --prefix <dir>      Target installation prefix (default: ${DEFAULT_PREFIX})
  --no-xdg            Skip writing XDG .desktop launcher and hicolor user icons
  --yes, -y           Non-interactive unattended installation
EOH
}

while [ $# -gt 0 ]; do
    case "$1" in
        --help|-h)
            print_help
            exit 0
            ;;
        --info)
            MODE="info"
            shift
            ;;
        --check)
            MODE="check"
            shift
            ;;
        --yes|-y)
            ASSUME_YES=1
            shift
            ;;
        --no-xdg)
            INSTALL_XDG=0
            shift
            ;;
        --prefix)
            if [ $# -lt 2 ]; then
                echo "Error: --prefix requires a directory argument" >&2
                exit 1
            fi
            PREFIX="$2"
            shift 2
            ;;
        --prefix=*)
            PREFIX="${1#*=}"
            shift
            ;;
        *)
            echo "Error: unknown option '$1' (try --help)" >&2
            exit 1
            ;;
    esac
done

if [ "${MODE}" = "info" ]; then
    echo "Package        : ${APP_NAME}"
    echo "Version        : ${APP_VERSION}"
    echo "Architecture   : ${APP_ARCH}"
    echo "Payload Bytes  : ${PAYLOAD_BYTES}"
    echo "Payload SHA256 : ${EXPECTED_SHA256}"
    echo "Default Prefix : ${DEFAULT_PREFIX}"
    exit 0
fi

ARCHIVE_LINE="$(awk '/^__WATERSCAPE_ARCHIVE_BELOW__$/ { print NR + 1; exit 0; }' "$0")"
if [ -z "${ARCHIVE_LINE}" ]; then
    echo "Error: missing embedded archive marker in $0" >&2
    exit 1
fi

compute_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        tail -n +"${ARCHIVE_LINE}" "$0" | sha256sum | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        tail -n +"${ARCHIVE_LINE}" "$0" | shasum -a 256 | awk '{print $1}'
    else
        echo "Error: neither sha256sum nor shasum found on PATH" >&2
        exit 1
    fi
}

ACTUAL_SHA256="$(compute_sha256)"
if [ "${ACTUAL_SHA256}" != "${EXPECTED_SHA256}" ]; then
    echo "Error: SHA-256 verification FAILED!" >&2
    echo "  Expected: ${EXPECTED_SHA256}" >&2
    echo "  Actual  : ${ACTUAL_SHA256}" >&2
    exit 1
fi

if [ "${MODE}" = "check" ]; then
    echo "SHA-256 checksum OK (${ACTUAL_SHA256})"
    exit 0
fi

if [ "${ASSUME_YES}" -eq 0 ] && [ -t 0 ]; then
    printf "Install %s %s into '%s'? [Y/n] " "${APP_NAME}" "${APP_VERSION}" "${PREFIX}"
    read -r ans || ans="y"
    case "${ans}" in
        ""|y|Y|yes|YES) ;;
        *)
            echo "Installation cancelled."
            exit 0
            ;;
    esac
fi

mkdir -p "${PREFIX}/bin" "${PREFIX}/lib/waterscape" "${PREFIX}/share"
PREFIX_ABS="$(cd "${PREFIX}" && pwd)"

tail -n +"${ARCHIVE_LINE}" "$0" | tar -C "${PREFIX_ABS}" -xzf -

# Generate <prefix>/bin/waterscape wrapper script
cat > "${PREFIX_ABS}/bin/waterscape" <<EOW
#!/bin/sh
set -eu
WS_LIB="${PREFIX_ABS}/lib/waterscape"
export LD_LIBRARY_PATH="\${WS_LIB}\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}"
exec "\${WS_LIB}/waterscape" --module-dir "\${WS_LIB}" "\$@"
EOW
chmod 0755 "${PREFIX_ABS}/bin/waterscape"

XDG_DESKTOP_INSTALLED=""
XDG_ICON256_INSTALLED=""
XDG_ICON48_INSTALLED=""
XDG_ICONSVG_INSTALLED=""

if [ "${INSTALL_XDG}" -eq 1 ]; then
    if [ "$(id -u)" = "0" ]; then
        XDG_DATA_ROOT="/usr/local/share"
    else
        XDG_DATA_ROOT="${XDG_DATA_HOME:-${HOME:-/tmp}/.local/share}"
    fi
    mkdir -p \
        "${XDG_DATA_ROOT}/applications" \
        "${XDG_DATA_ROOT}/icons/hicolor/256x256/apps" \
        "${XDG_DATA_ROOT}/icons/hicolor/48x48/apps" \
        "${XDG_DATA_ROOT}/icons/hicolor/scalable/apps"

    XDG_DESKTOP_INSTALLED="${XDG_DATA_ROOT}/applications/waterscape.desktop"
    XDG_ICON256_INSTALLED="${XDG_DATA_ROOT}/icons/hicolor/256x256/apps/waterscape.png"
    XDG_ICON48_INSTALLED="${XDG_DATA_ROOT}/icons/hicolor/48x48/apps/waterscape.png"
    XDG_ICONSVG_INSTALLED="${XDG_DATA_ROOT}/icons/hicolor/scalable/apps/waterscape.svg"

    sed "s|^Exec=waterscape %U|Exec=\"${PREFIX_ABS}/bin/waterscape\" %U|" \
        "${PREFIX_ABS}/share/applications/waterscape.desktop" > "${XDG_DESKTOP_INSTALLED}"
    chmod 0644 "${XDG_DESKTOP_INSTALLED}"

    cp "${PREFIX_ABS}/share/icons/hicolor/256x256/apps/waterscape.png" "${XDG_ICON256_INSTALLED}"
    cp "${PREFIX_ABS}/share/icons/hicolor/48x48/apps/waterscape.png" "${XDG_ICON48_INSTALLED}"
    cp "${PREFIX_ABS}/share/icons/hicolor/scalable/apps/waterscape.svg" "${XDG_ICONSVG_INSTALLED}"
fi

# Generate clean <prefix>/uninstall.sh script
cat > "${PREFIX_ABS}/uninstall.sh" <<EOU
#!/bin/sh
set -eu
echo "Uninstalling Waterscape Web Browser from '${PREFIX_ABS}'..."
if [ -n "${XDG_DESKTOP_INSTALLED}" ] && [ -f "${XDG_DESKTOP_INSTALLED}" ]; then
    rm -f "${XDG_DESKTOP_INSTALLED}"
fi
if [ -n "${XDG_ICON256_INSTALLED}" ] && [ -f "${XDG_ICON256_INSTALLED}" ]; then
    rm -f "${XDG_ICON256_INSTALLED}"
fi
if [ -n "${XDG_ICON48_INSTALLED}" ] && [ -f "${XDG_ICON48_INSTALLED}" ]; then
    rm -f "${XDG_ICON48_INSTALLED}"
fi
if [ -n "${XDG_ICONSVG_INSTALLED}" ] && [ -f "${XDG_ICONSVG_INSTALLED}" ]; then
    rm -f "${XDG_ICONSVG_INSTALLED}"
fi
rm -rf "${PREFIX_ABS}/bin/waterscape" "${PREFIX_ABS}/lib/waterscape" "${PREFIX_ABS}/share"
rm -f "${PREFIX_ABS}/uninstall.sh"
rmdir "${PREFIX_ABS}/bin" "${PREFIX_ABS}/lib" "${PREFIX_ABS}" 2>/dev/null || true
echo "Waterscape uninstalled cleanly."
EOU
chmod 0755 "${PREFIX_ABS}/uninstall.sh"

echo "Installed ${APP_NAME} ${APP_VERSION} to ${PREFIX_ABS}"
echo "  Launcher  : ${PREFIX_ABS}/bin/waterscape"
echo "  Uninstall : ${PREFIX_ABS}/uninstall.sh"
exit 0
__WATERSCAPE_ARCHIVE_BELOW__
EOF_RUN_TEMPLATE

cat "${TAR_GZ}" >> "${OUTPUT_RUN}"
chmod 0755 "${OUTPUT_RUN}"
echo "Built self-extracting installer: ${OUTPUT_RUN} (SHA256=${PAYLOAD_SHA256})"
