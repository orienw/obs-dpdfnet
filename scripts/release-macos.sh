#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# macOS release artifact staging: build -> install -> zip.
#
# CI runs this for every push to main and every release tag, after the build
# and tests; the release job in .github/workflows/windows.yml publishes its
# output. Run it locally to check a release build before tagging.
#
#   ./scripts/release-macos.sh <build-dir>
#
# The build directory must be configured with cmake/macos and the default
# install destinations, as the README describes.

set -euo pipefail

if [ $# -ne 1 ] || [ ! -f "$1/CMakeCache.txt" ]; then
  echo "usage: release-macos.sh <build-dir>" >&2
  exit 2
fi
build_dir="$1"

cached() {
  sed -n "s/^$1:[A-Z]*=//p" "$build_dir/CMakeCache.txt"
}

# Other destinations would not install the bundle the zip promises, and
# absolute ones would install outside the staging directory.
if [ "$(cached DPDFNET_PLUGIN_DESTINATION)" != obs-dpdfnet.plugin/Contents/MacOS ] ||
   [ "$(cached DPDFNET_DATA_DESTINATION)" != obs-dpdfnet.plugin/Contents/Resources ]; then
  echo "$build_dir does not use the default install destinations" >&2
  exit 1
fi
obs_config="$(cached OBS_SOURCE_DIR)/libobs/obs-config.h"
if [ ! -f "$obs_config" ]; then
  echo "$build_dir was not configured with cmake/macos" >&2
  exit 1
fi

# Bring the build up to date first, so the zip holds the version it is named
# for.
cmake --build "$build_dir"
version="$(tr -d '[:space:]' < "$(cached CMAKE_HOME_DIRECTORY)/VERSION")"

# The notes describe what the build used, not what the repository pins.
obs_api() {
  sed -n "s/^#define LIBOBS_API_$1_VER \\([0-9]*\\)\$/\\1/p" "$obs_config"
}
obs_version="$(obs_api MAJOR).$(obs_api MINOR).$(obs_api PATCH)"
if [[ ! "$obs_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "$obs_config does not state an OBS version" >&2
  exit 1
fi
# OBS compares only major.minor, so the zip runs on this OBS or any later one.
obs_major_minor="${obs_version%.*}"

# A fresh directory, so nothing from an earlier run can enter the zip.
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
cmake --install "$build_dir" --prefix "$staging"

runtime="$staging/obs-dpdfnet.plugin/Contents/Frameworks/libonnxruntime_dpdfnet.dylib"
if [ ! -f "$runtime" ]; then
  echo "$build_dir does not bundle ONNX Runtime" >&2
  exit 1
fi
# The library lists itself first.
versions="$(otool -L "$runtime" |
  sed -n 's/.*current version \([0-9.]*\)).*/\1/p')"
onnxruntime_version="${versions%%$'\n'*}"
if [ -z "$onnxruntime_version" ]; then
  echo "$runtime does not state an ONNX Runtime version" >&2
  exit 1
fi

cat > "$staging/INSTALL.txt" <<END_OF_TEXT
obs-dpdfnet $version - macOS, Apple silicon
DPDFNet local speech-enhancement audio filter for OBS Studio.

REQUIREMENTS
- A Mac with Apple silicon, macOS 14 or later
- OBS Studio $obs_major_minor or later
- OBS audio sample rate: 48 kHz preferred; 44.1 kHz is supported through internal resampling

INSTALL
1. Quit OBS Studio.
2. Copy "obs-dpdfnet.plugin" from this zip into:
       ~/Library/Application Support/obs-studio/plugins/
   Create the "plugins" folder if it does not exist.
3. This build is not notarized, so macOS blocks it until you clear the
   download flag. In Terminal:
       xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-dpdfnet.plugin
4. Start OBS, then add the filter:
       Audio Mixer -> mic gear -> Filters -> + -> DPDFNet Noise Suppression

NOTES
- Built against OBS Studio $obs_version and ONNX Runtime $onnxruntime_version.
- Single-channel speech enhancer. Use it on a microphone source, not on desktop
  audio or music. Pick the mic input channel explicitly on stereo sources.

LICENSING
- Plugin code: GPL-2.0-or-later. See LICENSE in the plugin's
  Contents/Resources folder.
- Bundled DPDFNet models: Apache-2.0. ONNX Runtime: MIT. KissFFT: BSD-3-Clause.
- Details: THIRD_PARTY.md, LICENSES/, ThirdPartyNotices.txt, and
  ONNXRuntime-LICENSE.txt in the same folder.

Source code: https://github.com/orienw/obs-dpdfnet (release tag v$version)
END_OF_TEXT

zip="$build_dir/obs-dpdfnet-$version-macos-arm64.zip"
rm -f "$zip"
ditto -c -k --norsrc --noextattr "$staging" "$zip"
echo "Staged $zip"
