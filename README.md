# obs-dpdfnet

An OBS Studio audio filter that removes background noise from your microphone
with DPDFNet, a neural speech-enhancement model. It runs on your CPU and makes
no network requests. It is tuned for a close dynamic microphone at 48 kHz.

Release builds are for Windows x64 and macOS on Apple silicon. Linux builds
from source.

## Install A Release Build

Download the zip for your system from
[GitHub releases](https://github.com/orienw/obs-dpdfnet/releases).

### Windows

Requirements: Windows 10/11 64-bit and OBS Studio 32.0 or later, x64.

The binary is unsigned, so SmartScreen or Defender may warn on download or
first load.

1. Close OBS.
2. Extract the zip and copy the `obs-dpdfnet` folder into
   `%ProgramData%\obs-studio\plugins\`.
3. Check that
   `%ProgramData%\obs-studio\plugins\obs-dpdfnet\bin\64bit\obs-dpdfnet.dll`
   exists.
4. Start OBS and add the filter:
   `Audio Mixer -> mic gear -> Filters -> + -> DPDFNet Noise Suppression`

To verify the download, compare this hash with the SHA-256 shown next to the
zip on the release page:

```powershell
Get-FileHash .\obs-dpdfnet-<version>-windows-x64.zip -Algorithm SHA256
```

### macOS

Requirements: a Mac with Apple silicon, macOS 14 or later, and OBS Studio 32.0
or later.

The plugin is not notarized, so macOS blocks it until you clear the download
flag in step 4.

1. Quit OBS.
2. OBS does not create its plugins folder. In Terminal, create it and open it
   in Finder:

   ```bash
   mkdir -p ~/Library/Application\ Support/obs-studio/plugins
   open ~/Library/Application\ Support/obs-studio/plugins
   ```

3. Extract the zip and drag `obs-dpdfnet.plugin` into that folder.
4. In Terminal, run:

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-dpdfnet.plugin
   ```

5. Start OBS and add the filter:
   `Audio Mixer -> mic gear -> Filters -> + -> DPDFNet Noise Suppression`

If OBS reports that obs-dpdfnet failed to load, run the step 4 command again
and restart OBS.

To verify the download, compare this hash with the SHA-256 shown next to the
zip on the release page:

```bash
shasum -a 256 obs-dpdfnet-<version>-macos-arm64.zip
```

## Settings

Use the filter on a microphone, not on desktop audio or music. Set OBS to
48 kHz in `Settings -> Audio`: the bundled models run natively at 48 kHz, and
other rates are resampled.

- `Model`: `DPDFNet8` sounds best. `DPDFNet2` uses less CPU. `Custom ONNX
  model` loads your own file; see [Custom models](#custom-models).
- `Input channel`: the channel your microphone is on, usually
  `Input 1 / left`. `Mix all channels` averages them. The enhanced voice goes
  to every output channel except LFE.
- `Suppression limit`: the most noise the filter removes. Start at 24 to 30 dB
  and raise it only if room noise is still obvious while you speak. 40 dB is
  aggressive, and 60 dB is a diagnostic extreme.
- `Mix`: the blend of enhanced and original audio. 100% is enhanced only.
- `Output gain`: gain after the blend. Start at 0 dB; positive values can
  clip.
- `Bypass`: passes the original audio at the same delay and keeps the model
  running, for A/B comparison. To stop the filter's CPU use, disable it in OBS
  instead.
- `Reset processing`: restarts processing if it was paused or turned off, and
  clears the model state, fault counters, and timing.

### Delay

With the bundled models, the filter delays audio by 50 ms: 40 ms in the model
and 10 ms of STFT overlap. The original audio in the mix, bypass, and
timestamps are aligned to the same delay, so OBS keeps the audio in sync with
video. Audio monitoring hears the delay.

### If The CPU Cannot Keep Up

When the computer cannot run the model in real time, the filter pauses noise
suppression and keeps passing audio at the same delay. The status line says
so, and the filter retries after 10 and 30 seconds, then every 60 seconds
until it keeps up. Suppression fades out over 100 ms and back in over 200 ms
instead of switching abruptly.

If pauses keep coming back, switch to `DPDFNet2` or reduce system load. If
processing fails repeatedly, it stays off until you press `Reset processing`.

### Status And Diagnostics

The `Status` line says what the filter is doing and what the listener hears.
Pauses and recovery show up on their own; press `Refresh` to update the
processing load and diagnostics.

`Show details` in the `Diagnostics` group shows measurements for the current
run:

- The active model, whether it runs natively or resampled, and its frame and
  hop sizes.
- The model and STFT delay, which excludes resampling and OBS buffering.
- Callback timing: the slowest callback and how many missed their deadline.
  Timing restarts on model, format, resampler, and reset changes, and does not
  count passthrough.
- Oversized packets, buffer-capacity hits, and timestamp jumps, kept until a
  reset or model change. A timestamp jump is audio arriving 70 ms or more away
  from where the previous packet ended, the point past which OBS stops
  smoothing timestamps. The filter then drops its buffered audio and starts
  over.

These are processing measurements, not end-to-end microphone latency.

## Models

`models/` holds the DPDFNet ONNX files from `Ceva-IP/DPDFNet`;
`models/manifest.json` records the source revision and SHA-256 hashes.

- `dpdfnet8_48khz_hr.onnx`: default, best quality, more CPU.
- `dpdfnet2_48khz_hr.onnx`: lighter alternative.

### Custom Models

A custom ONNX model must expose the DPDFNet two-input, two-output float32
tensor contract and declare integer `output_delay_hops` metadata from 0 to 16:
the model's spectral output delay, excluding STFT buffering and resampling.
Models that fail the contract or produce non-finite warm-up output are
rejected, and the active model stays loaded.

## Build From Source

### Windows

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

To refresh the pinned ONNX Runtime and model files with hash checks,
optionally rebuilding and installing:

```powershell
.\scripts\update-windows.ps1
.\scripts\update-windows.ps1 -Build -Install
```

Pass `-OnnxRuntimeVersion latest` to try a newer ONNX Runtime.

### CMake

CMake is the manual path for contributors, custom OBS builds, Linux, and
macOS. It needs CMake 3.24+, a C++17 compiler, OBS Studio 30 or later
development files with `libobsConfig.cmake`, and an ONNX Runtime package.
KissFFT is fetched at configure time unless `DPDFNET_FETCH_KISSFFT` is off.
Restart OBS after installing.

Windows:

```powershell
.\scripts\bootstrap-windows.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -Dlibobs_DIR="C:\path\to\obs-studio\build_x64\libobs" `
  -DONNXRUNTIME_ROOT="$PWD\third_party\onnxruntime"
cmake --build build --config Release
.\scripts\install-windows.ps1 -BuildDir .\build
```

Linux, installing into the OBS user plugin folder. Install `patchelf` first.
Point `libobs_DIR` at an OBS build tree instead if your OBS package ships no
CMake files.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -Dlibobs_DIR=/usr/lib/cmake/libobs \
  -DONNXRUNTIME_ROOT="/path/to/onnxruntime" \
  -DDPDFNET_PLUGIN_DESTINATION=bin/64bit -DDPDFNET_DATA_DESTINATION=data
cmake --build build
cmake --install build --prefix ~/.config/obs-studio/plugins/obs-dpdfnet
```

macOS on Apple silicon, installing into the OBS user plugin folder. It also
needs the SIMDe headers (`brew install simde`) and the ONNX Runtime
`osx-arm64` package.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
  -Dlibobs_DIR="$PWD/cmake/macos" \
  -DOBS_SOURCE_DIR="/path/to/obs-studio" \
  -DOBS_APP=/Applications/OBS.app \
  -DONNXRUNTIME_ROOT="/path/to/onnxruntime"
cmake --build build
cmake --install build --prefix ~/Library/Application\ Support/obs-studio/plugins
```

OBS publishes no development files for macOS, so `cmake/macos` stands in for
its CMake package: it takes the headers from an OBS source tree and libobs
from an OBS.app. Check out the source at the version of that app. The build
loads into that OBS version and later ones.

The install layout defaults to `obs-plugins/64bit` and
`data/obs-plugins/obs-dpdfnet`, and on macOS to an `obs-dpdfnet.plugin`
bundle. Override `DPDFNET_PLUGIN_DESTINATION` and `DPDFNET_DATA_DESTINATION`
for other layouts.

ONNX Runtime is copied next to built targets and installed with the plugin
unless `DPDFNET_COPY_RUNTIME_DEPENDENCIES` or
`DPDFNET_INSTALL_RUNTIME_DEPENDENCIES` is off. On Linux and macOS the build
installs it as `libonnxruntime_dpdfnet`, so it cannot clash with another OBS
plugin's copy; on Linux it uses `patchelf` for this. On Windows, a CMake
install keeps the stock `onnxruntime.dll` name, which can clash; share builds
made with the PowerShell scripts, which rename it.

`DPDFNET_BUILD_TESTS` registers the processor, model contract, libobs filter
lifecycle, and installed-plugin tests with CTest. The installed-plugin test
installs into the build tree and loads that copy the way OBS does.
`DPDFNET_BUILD_MODEL_SMOKE`, `DPDFNET_BUILD_STREAM_DUMP`,
`DPDFNET_BUILD_PROCESSOR_BENCHMARK`, and `DPDFNET_BUILD_QUALITY_BENCHMARK` build
the standalone tools. `DPDFNET_WARNINGS_AS_ERRORS` makes compiler warnings in
this project's own code fatal, as CI does.

## Tests And Benchmarks

On Linux and macOS, configure with `-DDPDFNET_BUILD_TESTS=ON` and run
`ctest --test-dir build`.

On Windows, the test script needs Python 3 for WAV input validation. It builds
the plugin and tools and runs every test:

```powershell
.\scripts\build-windows-msvc.ps1
.\scripts\test-windows.ps1
```

The tests cover both bundled models, malformed model contracts, variable
packet sizes, resampling, bypass transitions, timestamp jumps, the failure
circuit breakers, the realtime overload pause and retry, the filter lifecycle
against real libobs, and loading the installed plugin the way OBS does.

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

## License

GPL-2.0-or-later for the plugin source. The bundled models and downloaded
dependencies keep their upstream licenses; see `LICENSE`, `THIRD_PARTY.md`, and
`LICENSES/`.
