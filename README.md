# obs-dpdfnet

Native OBS audio filter for local DPDFNet speech enhancement.

`obs-dpdfnet` loads a streaming DPDFNet ONNX model, processes 10 ms mono voice
frames with ONNX Runtime, and returns the enhanced signal as a regular OBS
audio filter. It is tuned for a close dynamic microphone in a 48 kHz OBS setup.

Audio processing runs locally. The plugin does not make network requests at
runtime.

## Status

This is an early, working OBS plugin. The Windows path is the primary tested
path right now, including the direct MSVC helper scripts in `scripts/`.

Current filter:

- OBS filter name: `DPDFNet Noise Suppression`
- Default model: `models/dpdfnet8_48khz_hr.onnx`
- Model input: streaming DPDFNet ONNX with metadata-backed state initialization
- Audio path: one selected mono input, blended back to the source channels
- Controls: model path, input channel, suppression limit, wet mix, output gain,
  bypass, reset state

## Install A Release Build

Requirements:

- Windows 10/11 64-bit
- OBS Studio x64
- OBS audio sample rate set to 48 kHz

Download the Windows x64 zip and `.sha256` file from the
[GitHub releases](https://github.com/orienw/obs-dpdfnet/releases) page.

To install:

1. Close OBS Studio.
2. Extract `obs-dpdfnet-<version>-windows-x64.zip`.
3. Copy the extracted `obs-dpdfnet` folder into
   `%ProgramData%\obs-studio\plugins\`.
4. Confirm this file exists:
   `%ProgramData%\obs-studio\plugins\obs-dpdfnet\bin\64bit\obs-dpdfnet.dll`
5. Start OBS, then add the filter:
   `Audio Mixer -> mic gear -> Filters -> + -> DPDFNet Noise Suppression`

Optional checksum verification from PowerShell:

```powershell
Get-FileHash .\obs-dpdfnet-<version>-windows-x64.zip -Algorithm SHA256
```

## Quick Start On Windows

From PowerShell in this directory:

```powershell
.\scripts\bootstrap-windows.ps1
.\scripts\build-windows-msvc.ps1
.\scripts\install-windows.ps1 -BuildDir .\build\msvc
```

Restart OBS, then add the filter here:

`Audio Mixer -> mic gear -> Filters -> + -> DPDFNet Noise Suppression`

The helper scripts download third-party build inputs into `third_party/`, build
outputs into `build/`, and install the plugin under OBS's per-machine plugin
folder:

`%ProgramData%\obs-studio\plugins\obs-dpdfnet`

## Recommended Settings

For preserving an RE20-style close dynamic mic sound, start with:

- `Input channel`: `Input 1 / left`
- `Suppression limit`: `24-30 dB`
- `Wet mix`: `100%`
- `Output gain`: `0 dB`
- OBS sample rate: `48 kHz`

Raise the suppression limit only when the room noise is still obvious while
speaking. `40 dB` is aggressive, and `60 dB` is mostly useful as a diagnostic or
extreme setting.

This is a single-channel speech enhancer. On stereo sources, choose the mic
input channel explicitly or use `Mix all channels` only when that is really what
you want. Use it on a microphone source, not on desktop audio or music.

If the loaded model sample rate does not match OBS's audio sample rate, the
filter bypasses and logs a warning. The bundled models expect 48 kHz.

## CMake Build

Requirements:

- OBS Studio development files with `libobsConfig.cmake`
- CMake 3.24+
- Visual Studio 2022 on Windows, or a C++17 compiler on Linux/macOS
- ONNX Runtime C/C++ package
- Network access during CMake configure, unless you provide KissFFT yourself

The official OBS installer may not include the development CMake package. If
CMake cannot find `libobs`, build against an OBS source/build tree or an OBS
plugin development package.

Example Windows configure:

```powershell
.\scripts\bootstrap-windows.ps1

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -Dlibobs_DIR="C:\path\to\obs-studio\build_x64\libobs" `
  -DONNXRUNTIME_ROOT="$PWD\third_party\onnxruntime"

cmake --build build --config Release
.\scripts\install-windows.ps1 -BuildDir .\build
```

Optional standalone model smoke test:

```powershell
cmake -S . -B build-smoke -G "Visual Studio 17 2022" -A x64 `
  -DDPDFNET_BUILD_MODEL_SMOKE=ON `
  -Dlibobs_DIR="C:\path\to\obs-studio\build_x64\libobs" `
  -DONNXRUNTIME_ROOT="$PWD\third_party\onnxruntime"

cmake --build build-smoke --config Release --target dpdfnet-model-smoke
.\build-smoke\Release\dpdfnet-model-smoke.exe .\models\dpdfnet8_48khz_hr.onnx
```

## Models

The `models/` directory contains DPDFNet ONNX artifacts from
`Ceva-IP/DPDFNet`. `models/manifest.json` records the source revision, file
names, sizes, and SHA-256 hashes.

To refresh ONNX Runtime and the DPDFNet models with hash checks:

```powershell
.\scripts\update-windows.ps1
```

To refresh, rebuild, and install:

```powershell
.\scripts\update-windows.ps1 -Build -Install
```

## Release Workflow

The Windows release flow is split in two: Windows PowerShell builds and stages
the Windows artifact, then WSL publishes the GitHub tag and release with the
GitHub auth configured for this checkout.

From Windows PowerShell:

```powershell
.\scripts\release-windows.ps1 -Version 0.3.1 `
  -Changelog @(
    "Improved realtime audio processing."
    "Fixed a model reload edge case."
  )
```

That writes the zip, checksum, and release notes under `build/`.

From WSL:

```bash
./scripts/publish-release-wsl.sh 0.3.1
```

For a draft release, add `--draft` to the WSL publish command.

## License

The plugin source code is licensed under GPL-2.0-or-later. The bundled DPDFNet
model artifacts and downloaded build/runtime dependencies keep their upstream
licenses. See `LICENSE`, `THIRD_PARTY.md`, and `LICENSES/`.
