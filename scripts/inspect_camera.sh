#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
inspect_camera.sh - collect RK3588 camera bring-up evidence

Usage:
  ./scripts/inspect_camera.sh [--out FILE]

Examples:
  ./scripts/inspect_camera.sh --out camera_evidence.txt
  ./scripts/inspect_camera.sh > camera_evidence.txt
EOF
}

out=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)
      out="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -n "$out" ]]; then
  exec >"$out" 2>&1
fi

ts="$(date -Is 2>/dev/null || date)"
echo "# Camera evidence"
echo
echo "- collected_at: ${ts}"
echo "- uname: $(uname -a || true)"
echo

echo "## /proc/device-tree/model"
cat /proc/device-tree/model 2>/dev/null || echo "(missing)"
echo

echo "## /etc/os-release"
cat /etc/os-release 2>/dev/null || echo "(missing)"
echo

echo "## dmesg (camera related)"
dmesg | grep -Ei 'sensor|mipi|csi|isp|rkaiq|rkcif|cif|v4l2' || true
echo

echo "## media-ctl -p"
if command -v media-ctl >/dev/null 2>&1; then
  media-ctl -p
else
  echo "(media-ctl not found)"
fi
echo

echo "## v4l2-ctl --list-devices"
if command -v v4l2-ctl >/dev/null 2>&1; then
  v4l2-ctl --list-devices
else
  echo "(v4l2-ctl not found)"
fi
echo

echo "## /dev/video* nodes"
ls -l /dev/video* 2>/dev/null || echo "(no /dev/video* nodes)"
echo

echo "## formats (best-effort)"
if command -v v4l2-ctl >/dev/null 2>&1; then
  for dev in /dev/video*; do
    [[ -e "$dev" ]] || continue
    echo "### $dev --all"
    v4l2-ctl -d "$dev" --all || true
    echo
    echo "### $dev --list-formats-ext"
    v4l2-ctl -d "$dev" --list-formats-ext || true
    echo
  done
fi

