# obs-dpdfnet

Native OBS audio filter for local DPDFNet speech enhancement.

The plugin runs a streaming DPDFNet ONNX model with ONNX Runtime on the CPU,
enhances the selected mono channel in 10 ms hops, and returns the result as a
regular OBS audio filter. It is tuned for a close dynamic microphone at 48 kHz.
Everything runs locally; the plugin makes no network requests.

Tested on Windows x64 and Linux. Release builds are Windows only; Linux
builds from source with CMake.

## Install A Release Build

Requirements: Windows 10/11 64-bit and OBS Studio x64.

Download the zip and its `.sha256` file from
[GitHub releases](https://github.com/orienw/obs-dpdfnet/releases). The binary
is unsigned, so SmartScreen or Defender may warn on download or first load.

1. Close OBS.
2. Extract the zip and copy the `obs-dpdfnet` folder into
   `%ProgramData%\obs-studio\plugins\`.
3. Check that
   `%ProgramData%\obs-studio\plugins\obs-dpdfnet\bin\64bit\obs-dpdfnet.dll`
   exists.
4. Start OBS and add the filter:
   `Audio Mixer -> mic gear -> Filters -> + -> DPDFNet Noise Suppression`

To verify the download, the two hashes must match:

```powershell
Get-FileHash .\obs-dpdfnet-<version>-windows-x64.zip -Algorithm SHA256
Get-Content .\obs-dpdfnet-<version>-windows-x64.zip.sha256
```

## Settings

Start with:

- `Model`: `DPDFNet8 (best quality, more CPU)`
- `Input channel`: `Input 1 / left`
- `Suppression limit`: `24-30 dB`
- `Mix`: `100%`
- `Output gain`: `0 dB`
- OBS sample rate: `48 kHz` (the bundled models run natively, no resampling)

Raise the suppression limit only if room noise is still obvious while you
speak. `40 dB` is aggressive; `60 dB` is a diagnostic extreme.

Use the filter on a microphone, not on desktop audio or music. It enhances one
channel: on stereo sources pick the mic channel, or `Mix all channels` if you
really want the average. Other OBS sample rates work; the voice lane is
resampled to and from the model's 48 kHz.

`DPDFNet8` sounds best but costs more CPU. If the machine cannot keep up, the
filter pauses noise suppression, keeps passing audio at the same delay, says
so in the status line, and retries after 10, 30, and 60 seconds. The switch
in and out of the pause is seamless. If it still cannot keep up after that, or
processing fails repeatedly, processing stays off until you press
`Reset processing`. Switch to `DPDFNet2` or lower system load first.

`Bypass` passes the original audio, delay-matched to the processed path, and
keeps the model warm for A/B comparison. Disable the filter in OBS to stop its
CPU use.

The bundled models add 40 ms of internal delay. The filter aligns the dry mix,
bypass, and timestamps to that delay so every output describes the same input
audio.

Custom ONNX models must expose the DPDFNet two-input, two-output float32 tensor
contract and declare integer `output_delay_hops` metadata from 0 to 16: the
model's spectral output delay, excluding STFT buffering and resampling. Models
that fail the contract or produce non-finite warm-up output are rejected and
the active model stays loaded.

The `Status` line says what the filter is doing and what the listener hears.
`Show details` in the `Diagnostics` group reveals the processing measurements
for the current run: the active model, native or resampled operation, frame
and hop sizes, and callback timing. Timing restarts on model, format,
resampler, and reset changes; passthrough callbacks are not counted.
Oversized-packet and buffer-capacity counts persist until a reset or model
change. These are processing measurements, not end-to-end microphone latency.
Press `Refresh` to update them.

## Build From Source On Windows

From PowerShell in this directory:

```powershell
.\scripts\bootstrap-windows.ps1
.\scripts\build-windows-msvc.ps1
.\scripts\test-windows.ps1
.\scripts\install-windows.ps1 -BuildDir .\build\msvc
```

The scripts download build inputs into `third_party/`, build into `build/`,
and install into `%ProgramData%\obs-studio\plugins\obs-dpdfnet`. Restart OBS
afterwards.

## CMake Build

CMake is the manual path for contributors, custom OBS builds, Linux, and
macOS. It needs CMake 3.24+, a C++17 compiler, OBS Studio development
files with `libobsConfig.cmake`, and an ONNX Runtime package. KissFFT is
fetched at configure time unless `DPDFNET_FETCH_KISSFFT` is off.

Windows:

```powershell
.\scripts\bootstrap-windows.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -Dlibobs_DIR="C:\path\to\obs-studio\build_x64\libobs" `
  -DONNXRUNTIME_ROOT="$PWD\third_party\onnxruntime"
cmake --build build --config Release
.\scripts\install-windows.ps1 -BuildDir .\build
```

Linux/macOS, installing into the OBS user plugin folder:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -Dlibobs_DIR=/usr/lib/cmake/libobs \
  -DONNXRUNTIME_ROOT="/path/to/onnxruntime" \
  -DDPDFNET_PLUGIN_DESTINATION=bin/64bit -DDPDFNET_DATA_DESTINATION=data
cmake --build build
cmake --install build --prefix ~/.config/obs-studio/plugins/obs-dpdfnet
```

Point `libobs_DIR` at an OBS build tree instead if your OBS package ships no
CMake files. Restart OBS after installing.

The install layout defaults to `obs-plugins/64bit` and
`data/obs-plugins/obs-dpdfnet`; override `DPDFNET_PLUGIN_DESTINATION` and
`DPDFNET_DATA_DESTINATION` for other packages. ONNX Runtime shared libraries
are copied next to built targets and installed with the plugin unless
`DPDFNET_COPY_RUNTIME_DEPENDENCIES` or `DPDFNET_INSTALL_RUNTIME_DEPENDENCIES`
is off.

`DPDFNET_BUILD_TESTS` registers the processor, model contract, and libobs
filter lifecycle tests with CTest. `DPDFNET_BUILD_MODEL_SMOKE`,
`DPDFNET_BUILD_STREAM_DUMP`, `DPDFNET_BUILD_PROCESSOR_BENCHMARK`, and
`DPDFNET_BUILD_QUALITY_BENCHMARK` build the standalone tools.

## Tests And Benchmarks

The Windows gate builds the plugin and tools and runs every test:

```powershell
.\scripts\build-windows-msvc.ps1
.\scripts\test-windows.ps1
```

It covers both bundled models, malformed model contracts, variable packet
sizes, resampling, bypass transitions, the failure circuit breakers, and the
filter lifecycle against real libobs.

Processor timing for both models at 44.1, 48, and 96 kHz:

```powershell
.\scripts\benchmark-windows.ps1
```

The report lands in `build\processor-benchmark.txt`. Live callback timing is
shown in the filter's diagnostics.

The automated tests catch numerical and stream regressions; they do not
measure perceived quality. For that, build from a clean committed tree, put
mono 48 kHz clean-speech and noise WAVs under `build\quality-corpus\`, and run:

```powershell
.\scripts\quality-benchmark-windows.ps1 `
  -CaseName re20-fan-01 `
  -CleanWav .\build\quality-corpus\clean.wav `
  -NoiseWav .\build\quality-corpus\fan.wav
```

Results go to `build\quality-results\` per case, with listening WAVs and a
report of SI-SDR, noise attenuation, level, clipping, and provenance. Existing
results are kept unless you pass `-Overwrite`. Compare against a baseline from
the same corpus and listen before trusting a number.

## Models

`models/` holds the DPDFNet ONNX files from `Ceva-IP/DPDFNet`;
`models/manifest.json` records the source revision and SHA-256 hashes.

- `dpdfnet8_48khz_hr.onnx`: default, best quality, more CPU.
- `dpdfnet2_48khz_hr.onnx`: lighter alternative.

To refresh the pinned ONNX Runtime and model files with hash checks, optionally
rebuilding and installing:

```powershell
.\scripts\update-windows.ps1
.\scripts\update-windows.ps1 -Build -Install
```

Pass `-OnnxRuntimeVersion latest` to try a newer ONNX Runtime.

## Release Workflow

Windows PowerShell builds, tests, and stages the artifact; WSL or Linux
publishes the tag and GitHub release.

```powershell
.\scripts\release-windows.ps1 -Version <version> -Changelog @(
  "First change."
  "Second change."
)
```

The script rebuilds unless `-SkipBuild` is passed, always runs the test gate,
checks that the source tree is clean, and writes the zip, checksum, and notes
under `build/`. Pass `-ObsInstallDir` for a portable OBS install. CI stages the
same `windows-release` artifact for every push to `main`; download it into
`build/` in a clean checkout of that commit to publish it.

```bash
./scripts/publish-release-wsl.sh <version> [--draft]
```

## License

GPL-2.0-or-later for the plugin source. The bundled models and downloaded
dependencies keep their upstream licenses; see `LICENSE`, `THIRD_PARTY.md`, and
`LICENSES/`.
