#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <linux-platform> <release-directory>" >&2
  exit 2
fi

platform="$1"
release_dir="$2"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
mkdir -p "$release_dir"

for abi in 0.7 0.8; do
  "$script_dir/build-unix-module.sh" "$abi" "$staging" "$platform"
  archive="libSoapyAstra918Support-${platform}-abi${abi}.tar.gz"
  tar -czf "$release_dir/$archive" -C "$staging" "modules$abi"
done
