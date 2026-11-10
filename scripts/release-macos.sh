#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# macOS release artifact staging: install -> zip.
#
# CI runs this for every push to main and every release tag, after the build
# and tests; the release job in .github/workflows/windows.yml publishes its
# output. Run it locally to check a release build before tagging.
#
#   ./scripts/release-macos.sh <build-dir>

set -euo pipefail

if [ $# -ne 1 ] || [ ! -f "$1/CMakeCache.txt" ]; then
  echo "usage: release-macos.sh <build-dir>" >&2
  exit 2
fi
build_dir="$1"
root="$(cd "$(dirname "$0")/.." && pwd)"

pinned() {
  sed -n "s/^\\\$$1 = \"\\(.*\\)\"\$/\\1/p" "$root/scripts/dependency-versions.ps1"
}
version="$(tr -d '[:space:]' < "$root/VERSION")"
obs_version="$(pinned DpdfnetMinimumObsVersion)"
onnxruntime_version="$(pinned DpdfnetDefaultOnnxRuntimeVersion)"
if [ -z "$obs_version" ] || [ -z "$onnxruntime_version" ]; then
  echo "scripts/dependency-versions.ps1 pins no OBS or ONNX Runtime version" >&2
  exit 1
fi
# OBS compares only major.minor, so the zip runs on this OBS or any later one.
obs_major_minor="${obs_version%.*}"

# A fresh directory, so nothing from an earlier run can enter the zip.
staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT
cmake --install "$build_dir" --prefix "$staging"

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
