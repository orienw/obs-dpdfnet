# Building and Development

This guide covers building `obs-dpdfnet` from source, running tests and benchmarks, and using custom ONNX models.

CI and the Windows scripts download pinned ONNX Runtime archives from Microsoft's GitHub releases. If the download fails, they try npmmirror and verify the archive against the same SHA-256 before extracting it.

## Windows (PowerShell scripts)

From PowerShell in the repository root:

```powershell
.\scripts\bootstrap-windows.ps1
.\scripts\build-windows-msvc.ps1
.\scripts\test-windows.ps1
.\scripts\install-windows.ps1 -BuildDir .\build\msvc
```

The scripts download build inputs into `third_party/`, build into `build/`, and install into `%ProgramData%\obs-studio\plugins\obs-dpdfnet`. Restart OBS afterwards.

To refresh ONNX Runtime and verify the bundled models, optionally rebuilding and installing:

```powershell
.\scripts\update-windows.ps1
.\scripts\update-windows.ps1 -Build -Install
```

Pass `-OnnxRuntimeVersion latest` to try a newer ONNX Runtime release.
The updater restores missing or mismatched bundled models from the checkout's commit, or from the matching plugin tag in a release source archive.

## Manual CMake builds

Use CMake directly for Linux, macOS, custom OBS builds, or contributor workflows.

**Requirements:**
- CMake 3.24+
- C++17 compiler
- OBS Studio 30+ development files with `libobsConfig.cmake`
- An ONNX Runtime package
- KissFFT is fetched automatically at configure time unless `DPDFNET_FETCH_KISSFFT=OFF`.

Restart OBS after installing.

### Windows

```powershell
.\scripts\bootstrap-windows.ps1
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -Dlibobs_DIR="C:\path\to\obs-studio\build_x64\libobs" `
  -DONNXRUNTIME_ROOT="$PWD\third_party\onnxruntime"
cmake --build build --config Release
.\scripts\install-windows.ps1 -BuildDir .\build
```

### Linux

Install `patchelf` first so the bundled ONNX Runtime gets a private soname (see [ONNX Runtime library naming](#onnx-runtime-library-naming)). Point `libobs_DIR` at an OBS build tree if your distro package does not ship CMake files.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -Dlibobs_DIR=/usr/lib/cmake/libobs \
  -DONNXRUNTIME_ROOT="/path/to/onnxruntime" \
  -DDPDFNET_PLUGIN_DESTINATION=bin/64bit \
  -DDPDFNET_DATA_DESTINATION=data
cmake --build build
cmake --install build --prefix ~/.config/obs-studio/plugins/obs-dpdfnet
```

### macOS (Apple silicon)

Requires SIMDe headers (`brew install simde`) and the ONNX Runtime `osx-arm64` package.

OBS does not publish development files for macOS, so `cmake/macos` stands in for its CMake package by reading headers from an OBS source tree and linking `libobs` from `OBS.app`. Check out the OBS source tree matching that app version; the resulting plugin loads into that OBS version and later ones.

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

### CMake options

| Option | Default | Description |
| --- | --- | --- |
| `DPDFNET_FETCH_KISSFFT` | `ON` | Fetch KissFFT at configure time. |
| `DPDFNET_PLUGIN_DESTINATION` | `obs-plugins/64bit` (`obs-dpdfnet.plugin/Contents/MacOS` on macOS) | Install destination for the plugin binary. |
| `DPDFNET_DATA_DESTINATION` | `data/obs-plugins/obs-dpdfnet` (`obs-dpdfnet.plugin/Contents/Resources` on macOS) | Install destination for plugin data files. |
| `DPDFNET_COPY_RUNTIME_DEPENDENCIES` | `ON` | Copy ONNX Runtime shared libraries next to built targets. |
| `DPDFNET_INSTALL_RUNTIME_DEPENDENCIES` | `ON` | Install ONNX Runtime shared libraries alongside the plugin. |
| `DPDFNET_BUILD_TESTS` | `OFF` | Register processor, model contract, libobs lifecycle, and installed-plugin tests with CTest. |
| `DPDFNET_BUILD_MODEL_SMOKE` | `OFF` | Build the standalone model smoke tool. |
| `DPDFNET_BUILD_STREAM_DUMP` | `OFF` | Build the standalone stream parity dumper. |
| `DPDFNET_BUILD_PROCESSOR_BENCHMARK` | `OFF` | Build the processor timing benchmark. |
| `DPDFNET_BUILD_QUALITY_BENCHMARK` | `OFF` | Build the local perceptual quality benchmark. |
| `DPDFNET_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings in this project's own code as errors (matches CI). |

### ONNX Runtime library naming

By default, ONNX Runtime is copied next to built targets and installed with the plugin.

- **Linux and macOS:** The build renames the bundled library to `libonnxruntime_dpdfnet` (via `patchelf` on Linux, `install_name_tool` + ad-hoc `codesign` on macOS) so it cannot clash with another OBS plugin's ONNX Runtime.
- **Windows:** A raw CMake install keeps the stock `onnxruntime.dll` name, which can clash with other plugins. When sharing Windows builds, use the PowerShell scripts, which rename it.

## Tests and benchmarks

### Automated tests

- **Linux and macOS:** Configure with `-DDPDFNET_BUILD_TESTS=ON`, build, and run `ctest --test-dir build`.
- **Windows:** Requires Python 3 for WAV input validation. Run:

  ```powershell
  .\scripts\build-windows-msvc.ps1
  .\scripts\test-windows.ps1
  ```

The test suite covers both bundled models, malformed model contracts, variable packet sizes, resampling, bypass transitions, timestamp jumps, failure circuit breakers, realtime overload pause/retry, filter lifecycle against real `libobs`, and loading the installed plugin from the build tree the way OBS does.

### Processor timing benchmark

Measures processor timing for both models at 44.1, 48, and 96 kHz and writes the report to `build\processor-benchmark.txt`:

```powershell
.\scripts\benchmark-windows.ps1
```

Live callback timing is available directly in the filter's diagnostics panel.

### Perceptual quality benchmark

Automated tests catch numerical and stream regressions, not perceived audio quality. To evaluate quality, build from a clean committed tree, place mono 48 kHz clean-speech and noise WAV files under `build\quality-corpus\`, and run:

```powershell
.\scripts\quality-benchmark-windows.ps1 `
  -CaseName re20-fan-01 `
  -CleanWav .\build\quality-corpus\clean.wav `
  -NoiseWav .\build\quality-corpus\fan.wav
```

Results are written per case under `build\quality-results\`, including listening WAVs and a report of SI-SDR, noise attenuation, level, clipping, and provenance. Existing results are preserved unless you pass `-Overwrite`. Compare against a baseline from the same corpus and listen before trusting a number.

## Models

[models/](models/) contains the bundled FP32 models derived from Ceva's 48 kHz ONNX exports. The plugin keeps frequency bins and channels in one layout across adjacent DPRNN blocks. The trained weights are preserved. [models/manifest.json](models/manifest.json) identifies the Hugging Face revision, upstream file and digest (`upstreamSha256`), and bundled digest (`sha256`).

- `dpdfnet8_48khz_hr.onnx` — default, best quality, higher CPU usage.
- `dpdfnet2_48khz_hr.onnx` — lighter CPU alternative.

To reproduce a bundled model, install `onnx==1.21.0` in a Python environment and pass the pinned upstream file to the converter:

```bash
python scripts/optimize-model.py upstream/dpdfnet8_48khz_hr.onnx models/dpdfnet8_48khz_hr.onnx
```

Release builds compare the original and bundled models on a pinned Ceva noisy-speech recording, check audio and recurrent-state snapshots byte for byte, and report median and p99 processor timing. To run the same check locally, build the stream dump and processor benchmark and run `python tests/compare-bundled-models.py <binary-directory> <work-directory> --regenerate`. On Windows include the OBS runtime's `bin/64bit` directory on `PATH`.

### Custom models

A custom ONNX model must:

- Expose the DPDFNet two-input, two-output `float32` tensor contract.
- Declare integer `output_delay_hops` metadata from `0` to `16` (the model's spectral output delay, excluding STFT buffering and resampling).

Models that violate the contract or produce non-finite warm-up output are rejected, and the currently active model remains loaded.
