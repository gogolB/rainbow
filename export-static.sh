#!/bin/sh
set -eu

IMAGE=${1:-rainbow:static}
OUT=${2:-./rainbow}

usage() {
  echo "Usage: $0 [image[:tag]] [output_path]" >&2
  echo "Default image: rainbow:static" >&2
  echo "Default output: ./rainbow" >&2
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  usage
  exit 0
fi

cid=$(docker create "$IMAGE")
cleanup() {
  docker rm -f "$cid" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

docker cp "$cid:/rainbow" "$OUT"
chmod +x "$OUT" 2>/dev/null || true

check_static() {
  if command -v ldd >/dev/null 2>&1; then
    ldd_out=$(ldd "$OUT" 2>&1 || true)
    case "$ldd_out" in
      *"not a dynamic executable"*|*"statically linked"*)
        return 0
        ;;
      *)
        echo "ldd output indicates dynamic deps:" >&2
        echo "$ldd_out" >&2
        return 1
        ;;
    esac
  fi

  if command -v readelf >/dev/null 2>&1; then
    if readelf -d "$OUT" 2>/dev/null | grep -q "NEEDED"; then
      echo "readelf found NEEDED entries (dynamic deps):" >&2
      readelf -d "$OUT" | grep "NEEDED" >&2 || true
      return 1
    fi
    return 0
  fi

  echo "Warning: ldd/readelf not available; cannot verify static linkage." >&2
  return 0
}

if check_static; then
  echo "Exported $IMAGE:/rainbow to $OUT (static binary verified)"
else
  echo "Exported $IMAGE:/rainbow to $OUT (static verification failed)" >&2
  exit 1
fi
