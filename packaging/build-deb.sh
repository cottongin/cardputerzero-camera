#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PKG_FILENAME="czcamera_1.0.0_m5stack1_arm64.deb"
OUT_DIR="${SCRIPT_DIR}/dist"
STAGE_ONLY_DIR=""
LOCAL_BUILD=0
CZ_BIN=""

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]
  --local                 Build czcamera and .deb directly on Linux ARM64 host
  --stage-only <dir>      Stage .deb directory tree into <dir> and exit
  --bin <path>            Path to prebuilt czcamera binary
  --ui-bin <path>         Alias for --bin
  --out-dir <dir>         Output directory for .deb (default: packaging/dist)
  -h, --help              Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --local)
            LOCAL_BUILD=1
            shift
            ;;
        --stage-only)
            [[ $# -ge 2 ]] || { echo "Missing argument for --stage-only" >&2; exit 2; }
            STAGE_ONLY_DIR="$2"
            shift 2
            ;;
        --bin|--ui-bin)
            [[ $# -ge 2 ]] || { echo "Missing argument for $1" >&2; exit 2; }
            CZ_BIN="$2"
            shift 2
            ;;
        --out-dir)
            [[ $# -ge 2 ]] || { echo "Missing argument for --out-dir" >&2; exit 2; }
            OUT_DIR="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

stage_package_tree() {
    local stage_dir="$1"
    local cz_bin="$2"

    rm -rf "${stage_dir}"
    install -d "${stage_dir}/DEBIAN"
    install -d "${stage_dir}/usr/bin"
    install -d "${stage_dir}/usr/lib/czcamera/czcamera_cam"
    install -d "${stage_dir}/usr/share/APPLaunch/applications"
    install -d "${stage_dir}/usr/share/APPLaunch/share/images"

    install -m 0644 "${SCRIPT_DIR}/debian/control" "${stage_dir}/DEBIAN/control"
    install -m 0755 "${SCRIPT_DIR}/debian/postinst" "${stage_dir}/DEBIAN/postinst"
    install -m 0755 "${SCRIPT_DIR}/debian/prerm" "${stage_dir}/DEBIAN/prerm"

    install -m 0755 "${cz_bin}" "${stage_dir}/usr/bin/czcamera"
    install -m 0755 "${ROOT_DIR}/cam-helper/bin/czcamera-cam" "${stage_dir}/usr/bin/czcamera-cam"

    while IFS= read -r -d '' py_file; do
        local rel_path="${py_file#${ROOT_DIR}/cam-helper/czcamera_cam/}"
        install -d "${stage_dir}/usr/lib/czcamera/czcamera_cam/$(dirname "${rel_path}")"
        install -m 0644 "${py_file}" "${stage_dir}/usr/lib/czcamera/czcamera_cam/${rel_path}"
    done < <(find "${ROOT_DIR}/cam-helper/czcamera_cam" -type f -name '*.py' -print0)

    install -m 0644 "${SCRIPT_DIR}/czcamera.desktop" "${stage_dir}/usr/share/APPLaunch/applications/czcamera.desktop"
    install -m 0644 "${SCRIPT_DIR}/czcamera.png" "${stage_dir}/usr/share/APPLaunch/share/images/czcamera.png"

    for font_dir in "${ROOT_DIR}/app/assets/fonts" "${SCRIPT_DIR}/fonts"; do
        if [[ -d "${font_dir}" ]]; then
            while IFS= read -r -d '' font_file; do
                install -d "${stage_dir}/usr/share/czcamera/fonts"
                install -m 0644 "${font_file}" "${stage_dir}/usr/share/czcamera/fonts/$(basename "${font_file}")"
            done < <(find "${font_dir}" -maxdepth 1 -type f \( -name '*.ttf' -o -name '*.otf' \) -print0)
        fi
    done
}

resolve_cz_bin() {
    if [[ -n "${CZ_BIN}" ]]; then
        [[ -f "${CZ_BIN}" ]] || { echo "Specified binary not found: ${CZ_BIN}" >&2; exit 1; }
        echo "${CZ_BIN}"
        return
    fi
    for candidate in "${ROOT_DIR}/app/build-arm64/czcamera" "${ROOT_DIR}/app/build/czcamera"; do
        if [[ -f "${candidate}" ]]; then
            echo "${candidate}"
            return
        fi
    done
    echo ""
}

if [[ -n "${STAGE_ONLY_DIR}" ]]; then
    RESOLVED_BIN="$(resolve_cz_bin)"
    [[ -n "${RESOLVED_BIN}" ]] || { echo "czcamera binary not found; pass --bin <path> (or --ui-bin <path>)" >&2; exit 1; }
    stage_package_tree "${STAGE_ONLY_DIR}" "${RESOLVED_BIN}"
    echo "Staged package tree at ${STAGE_ONLY_DIR}"
    exit 0
fi

if [[ "${LOCAL_BUILD}" -eq 1 ]]; then
    if [[ -z "${CZ_BIN}" ]]; then
        BUILD_DIR="${ROOT_DIR}/app/build-arm64"
        cmake -S "${ROOT_DIR}/app" -B "${BUILD_DIR}" \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_CXX_FLAGS_RELEASE="-O2" \
            -DCZ_BUILD_APP=ON \
            -DCZ_TARGET_DEVICE=ON \
            -DCZ_BUILD_TESTS=ON
        cmake --build "${BUILD_DIR}" -j"$(nproc 2>/dev/null || echo 2)"
        ctest --test-dir "${BUILD_DIR}" --output-on-failure
        CZ_BIN="${BUILD_DIR}/czcamera"
    fi

    TMP_STAGE="$(mktemp -d)"
    trap 'rm -rf "${TMP_STAGE}"' EXIT
    stage_package_tree "${TMP_STAGE}/pkg" "${CZ_BIN}"

    mkdir -p "${OUT_DIR}"
    dpkg-deb --build --root-owner-group "${TMP_STAGE}/pkg" "${OUT_DIR}/${PKG_FILENAME}"
    echo "Built ${OUT_DIR}/${PKG_FILENAME}"
    exit 0
fi

# Default: build inside linux/arm64 Debian Bookworm Docker container
IMAGE_TAG="czcamera-deb-builder:arm64"
docker build --platform linux/arm64 -f "${SCRIPT_DIR}/Dockerfile.arm64" -t "${IMAGE_TAG}" "${ROOT_DIR}"

mkdir -p "${OUT_DIR}"
CONTAINER_ID="$(docker create --platform linux/arm64 "${IMAGE_TAG}")"
trap 'docker rm -f "${CONTAINER_ID}" >/dev/null 2>&1 || true' EXIT
docker cp "${CONTAINER_ID}:/app/packaging/dist/${PKG_FILENAME}" "${OUT_DIR}/${PKG_FILENAME}"
echo "Built ${OUT_DIR}/${PKG_FILENAME}"
