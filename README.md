# obs-dpdfnet

Native OBS audio filter for local DPDFNet speech enhancement.

`obs-dpdfnet` loads a streaming DPDFNet ONNX model, processes 10 ms mono voice
frames with ONNX Runtime, and returns the enhanced signal as a regular OBS
audio filter. It is tuned for a close dynamic microphone in a 48 kHz OBS setup.

Audio processing runs locally. The plugin does not make network requests at
runtime.

## Status

This is the `1.0.0` release. Windows x64 is the primary tested path, including
the direct MSVC helper scripts in `scripts/`.

Current filter:

- OBS filter name: `DPDFNet Noise Suppression`
- Default model: `models/dpdfnet8_48khz_hr.onnx`
- Model input: streaming DPDFNet ONNX with metadata-backed state initialization
- Audio path: one selected mono input, blended back to the source channels
- Controls: model preset or custom model, input channel, suppression limit, wet
  mix, output gain, latency-aligned bypass, retry/reset processing, diagnostics

## Install A Release Build

Requirements:

- Windows 10/11 64-bit
- OBS Studio x64

The release binary is unsigned. Windows SmartScreen or Defender may warn when
it is downloaded or first loaded by OBS.

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
Get-Content .\obs-dpdfnet-<version>-windows-x64.zip.sha256
```

The two SHA-256 values must match exactly.

## Build From Source On Windows

From PowerShell in this directory:

```powershell
.\scripts\bootstrap-windows.ps1
.\scripts\build-windows-msvc.ps1
.\scripts\test-windows.ps1
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
- `Model`: `Best quality (DPDFNet8)`
- `Suppression limit`: `24-30 dB`
- `Wet mix`: `100%`
- `Output gain`: `0 dB`
- OBS sample rate: `48 kHz` preferred (runs the model natively, no resampling)

Raise the suppression limit only when the room noise is still obvious while
speaking. `40 dB` is aggressive, and `60 dB` is mostly useful as a diagnostic or
extreme setting.

This is a single-channel speech enhancer. On stereo sources, choose the mic
input channel explicitly or use `Mix all channels` only when that is really what
you want. Use it on a microphone source, not on desktop audio or music.

If OBS's audio sample rate differs from the loaded model's rate (the bundled
models run at 48 kHz), the filter resamples the enhanced voice lane internally
at both boundaries, so a 44.1 kHz OBS setup works out of the box. At 48 kHz the
model runs natively with no resampling in the path.

`Bypass (latency-aligned)` keeps the processing pipeline warm and returns its
aligned dry lane. This prevents stale or reordered packets while comparing the
processed and original signals. Disable the filter with OBS's filter toggle
when the goal is to stop its CPU use completely.

The bundled models have four hops (40 ms) of internal signal delay. The filter
aligns the suppression blend to that delay and discards startup output so the
enhanced signal, dry mix, bypass, and timestamps describe the same input audio.
The quality benchmark also receives this aligned output. Its corrected scores
should not be compared directly with reports from versions before 1.0.1.

Custom ONNX models must declare integer `output_delay_hops` metadata between
0 and 16. Set it to the model's spectral output delay, excluding STFT buffering
and resampling. The legacy DPDFNet version 1 `dpdfnet2_48khz_hr` profile used by
both bundled models is recognized as four hops when that metadata is absent.
Other models without a declared delay are rejected instead of mixing
unaligned audio.

The diagnostics row distinguishes the selected model from the model that is
actually active, reports native or resampled operation, and shows the model's
frame and hop sizes. It also reports callback timing for the current active
processing epoch after audio has flowed. Model, format, resampler, and reset
transitions start a new epoch, and fail-open passthrough callbacks are excluded.
These figures are processing measurements, not an end-to-end microphone
latency claim. Press `Update status and diagnostics` while the filter properties
are open to refresh it. The row also retains oversized-packet and unexpected
buffer-capacity counts until a processing reset or model replacement, so a
fail-open discontinuity is not silent. Packets above the bounded 8,192-frame
realtime limit pass through unchanged.

## CMake Build

The PowerShell scripts above are the tested Windows release path. CMake is the
manual source-build path for contributors, custom OBS development builds, and
Linux/macOS experiments.

Requirements:

- OBS Studio development files with `libobsConfig.cmake`
- CMake 3.24+
- Visual Studio 2022 on Windows, or a C++17 compiler on Linux/macOS
- ONNX Runtime C/C++ package
- Network access during CMake configure, unless you provide KissFFT yourself

The official OBS installer may not include the development CMake package. If
CMake cannot find `libobs`, build against an OBS source/build tree or an OBS
plugin development package.

By default, CMake installs into OBS's source-build style layout:
`obs-plugins/64bit` and `data/obs-plugins/obs-dpdfnet`. Override
`DPDFNET_PLUGIN_DESTINATION` and `DPDFNET_DATA_DESTINATION` if your OBS package
uses different paths. CMake also copies the ONNX Runtime shared libraries it
finds next to built targets and installs them with the plugin; disable that with
`-DDPDFNET_COPY_RUNTIME_DEPENDENCIES=OFF` or
`-DDPDFNET_INSTALL_RUNTIME_DEPENDENCIES=OFF`.

Example Windows configure:

```powershell
.\scripts\bootstrap-windows.ps1

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -Dlibobs_DIR="C:\path\to\obs-studio\build_x64\libobs" `
  -DONNXRUNTIME_ROOT="$PWD\third_party\onnxruntime"

cmake --build build --config Release
.\scripts\install-windows.ps1 -BuildDir .\build
```

Example Linux/macOS configure:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -Dlibobs_DIR="/path/to/obs-studio/build/libobs" \
  -DONNXRUNTIME_ROOT="/path/to/onnxruntime"

cmake --build build
cmake --install build --prefix "/path/to/obs-prefix"
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

Set `DPDFNET_BUILD_TESTS=ON` to register the deterministic processor, model
contract, and real-libobs filter lifecycle tests with CTest. The other optional
tool targets are controlled by `DPDFNET_BUILD_STREAM_DUMP`,
`DPDFNET_BUILD_QUALITY_BENCHMARK`, and `DPDFNET_BUILD_PROCESSOR_BENCHMARK`.

## Tests And Benchmarks

The direct Windows build produces the plugin, both model checks, the processor
suite, and the benchmark tools. Run the authoritative local gate with:

```powershell
.\scripts\build-windows-msvc.ps1
.\scripts\test-windows.ps1
```

The suite covers both bundled models, malformed ONNX contracts, variable OBS
packet sizes, channel and timestamp resets, aligned bypass transitions,
44.1 and 96 kHz resampling, format transitions, resampled failure recovery,
deterministic signal-integrity cases, and the runtime failure circuit breaker.
Its real-libobs gate also checks filter lifecycle overlap, returned-buffer
lifetime, plugin-owned callback allocations, and synchronous callback logging.
The release script runs this gate before staging, including when `-SkipBuild`
is used, and requires hash-bound build metadata for the source revision,
versions, runtime dependencies, and tested artifacts.

Measure processor timing for both models at 44.1, 48, and 96 kHz with:

```powershell
.\scripts\benchmark-windows.ps1
```

The report is written to `build\processor-benchmark.txt`. Live OBS callback
timing, including lock wait, is accumulated by the filter and displayed in its
diagnostics row. Use a Windows heap or ETW trace when validating allocations in
ONNX Runtime and OBS themselves; plugin-owned buffer-capacity checks cannot see
allocations inside those libraries.

The automated signal tests detect numerical and stream-integrity regressions,
but they do not claim to measure perceived speech quality. For a real-corpus
comparison, commit and rebuild a clean source tree, put local mono 48 kHz PCM16
or float32 clean-speech and noise WAVs under `build\quality-corpus\`, then run:

```powershell
.\scripts\quality-benchmark-windows.ps1 `
  -CaseName re20-fan-01 `
  -CleanWav .\build\quality-corpus\clean.wav `
  -NoiseWav .\build\quality-corpus\fan.wav
```

The case name creates a separate result directory and existing evidence is not
overwritten unless `-Overwrite` is explicit. The ignored
`build\quality-results\` directory receives the exact scaled clean and noise
references, mixture and enhanced listening WAVs, and a report containing build,
runtime, executable, input, and model provenance plus the mixing scales,
settings, packet pattern, SI-SDR signals, clean-speech level change, noise
attenuation, peak, clipping, non-finite, and DC measurements. Compare each model
only with a baseline made from the same corpus and listen to every changed case
for pumping, musical noise, coloration, and speech transitions before accepting
it.

## Models

The `models/` directory contains DPDFNet ONNX artifacts from
`Ceva-IP/DPDFNet`. `models/manifest.json` records the source revision, file
names, sizes, and SHA-256 hashes.

The release includes two 48 kHz models:

- `dpdfnet8_48khz_hr.onnx` is the default, higher-capacity model.
- `dpdfnet2_48khz_hr.onnx` is the lighter alternative when lower CPU use is
  more important.

Choose `Best quality (DPDFNet8)`, `Lower CPU (DPDFNet2)`, or `Custom ONNX
model` in the filter properties. Existing scenes that stored a model path are
migrated without discarding custom or missing paths. Custom models must expose
the exact two-input, two-output float32 DPDFNet tensor contract described by
their metadata. Contract violations and non-finite warm-up output are rejected
before activation. Non-finite runtime output fails open and trips the circuit
breaker after repeated errors. Processing time is also compared with the amount
of model audio completed. Sustained overload opens a separate realtime circuit
and passes audio through until processing is reset or the model is replaced;
choose the lower-CPU model or a faster custom model if it repeats.

To refresh the pinned ONNX Runtime and DPDFNet model artifacts with hash checks:

```powershell
.\scripts\update-windows.ps1
```

To probe a newer ONNX Runtime release, pass `-OnnxRuntimeVersion latest`.

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
.\scripts\release-windows.ps1 -Version 1.0.0 `
  -Changelog @(
    "Reorganized settings into clear processing and diagnostics groups."
    "Added concise status reporting, control help, and recovery guidance."
    "Updated the build and test baseline to OBS Studio 32.2.1."
  )
```

The script rebuilds unless `-SkipBuild` is supplied, runs the mandatory Windows
test gate either way, verifies clean source and test provenance, then writes the
zip, checksum, and release notes under `build/`.

From WSL:

```bash
./scripts/publish-release-wsl.sh 1.0.0
```

For a draft release, add `--draft` to the WSL publish command.

## License

The plugin source code is licensed under GPL-2.0-or-later. The bundled DPDFNet
model artifacts and downloaded build/runtime dependencies keep their upstream
licenses. See `LICENSE`, `THIRD_PARTY.md`, and `LICENSES/`.
