#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
build_sdk.sh - cross-build entry point

Usage:
  ./scripts/build_sdk.sh --toolchain FILE --sysroot DIR --build-dir DIR [--generator NAME] [--target NAME]
EOF
}

toolchain=""
sysroot=""
build_dir=""
generator=""
target=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --toolchain) toolchain="${2:-}"; shift 2 ;;
    --sysroot) sysroot="${2:-}"; shift 2 ;;
    --build-dir) build_dir="${2:-}"; shift 2 ;;
    --generator) generator="${2:-}"; shift 2 ;;
    --target) target="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$toolchain" || -z "$sysroot" || -z "$build_dir" ]]; then
  echo "Missing required args." >&2
  usage >&2
  exit 2
fi

cmake_args=(
  -S .
  -B "${build_dir}"
  -DCMAKE_TOOLCHAIN_FILE="${toolchain}"
  -DCMAKE_SYSROOT="${sysroot}"
  -DROCKCHIP_SDK_SYSROOT="${sysroot}"
  -DBUILD_TESTING=ON
)
if [[ -n "$generator" ]]; then
  cmake_args+=(-G "${generator}")
fi

cmake "${cmake_args[@]}"
if [[ -n "$target" ]]; then
  cmake --build "${build_dir}" --target "${target}"
else
  cmake --build "${build_dir}"
fi
