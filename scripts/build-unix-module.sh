#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <0.7|0.8> <output-directory> <target-name>" >&2
  exit 2
fi

abi="$1"
output_dir="$2"
target_name="$3"
case "$abi" in
  0.7) soapysdr_tag=soapy-sdr-0.7.1 ;;
  0.8) soapysdr_tag=soapy-sdr-0.8.1 ;;
  *) echo "unsupported SoapySDR ABI: $abi" >&2; exit 2 ;;
esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
sdk_prefix="$work_dir/soapy-sdk"
module_prefix="$work_dir/module-install"
mkdir -p "$output_dir"

git clone --quiet --depth 1 --branch "$soapysdr_tag" \
  https://github.com/pothosware/SoapySDR.git "$work_dir/SoapySDR"

soapy_cmake_args=(
  -S "$work_dir/SoapySDR"
  -B "$work_dir/soapy-build"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="$sdk_prefix"
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  -DENABLE_LIBRARY=ON
  -DENABLE_APPS=OFF
  -DENABLE_TESTS=OFF
  -DENABLE_DOCS=OFF
  -DENABLE_PYTHON=OFF
  -DENABLE_PYTHON3=OFF
)
if [[ -n "${CMAKE_OSX_ARCHITECTURES:-}" ]]; then
  soapy_cmake_args+=("-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
  soapy_cmake_args+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}")
fi
cmake "${soapy_cmake_args[@]}"
cmake --build "$work_dir/soapy-build" --parallel "${BUILD_JOBS:-2}"
cmake --install "$work_dir/soapy-build"

soapy_config="$(find "$sdk_prefix" -name SoapySDRConfig.cmake -print -quit)"
if [[ -z "$soapy_config" ]]; then
  echo "SoapySDR $abi CMake package was not installed" >&2
  exit 1
fi

driver_cmake_args=(
  -S "$repo_root"
  -B "$work_dir/driver-build"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX="$module_prefix"
  -DBUILD_TESTING=OFF
  "-DSoapySDR_DIR=$(dirname "$soapy_config")"
)
if [[ -n "${CMAKE_OSX_ARCHITECTURES:-}" ]]; then
  driver_cmake_args+=("-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
  driver_cmake_args+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}")
fi
cmake "${driver_cmake_args[@]}"
cmake --build "$work_dir/driver-build" --parallel "${BUILD_JOBS:-2}"
cmake --install "$work_dir/driver-build"

module="$(find "$module_prefix" -type f \( \
  -name 'libSoapyAstra918Support.so' -o \
  -name 'libSoapyAstra918Support.dylib' \
\) -print -quit)"
if [[ -z "$module" ]]; then
  echo "SoapySDR ABI $abi module was not installed" >&2
  exit 1
fi

mkdir -p "$output_dir/modules$abi"
cp "$module" "$output_dir/modules$abi/"
if command -v file >/dev/null 2>&1; then
  file "$output_dir/modules$abi/$(basename "$module")"
fi
echo "Built SoapySDR ABI $abi module for $target_name: $output_dir/modules$abi/$(basename "$module")"
