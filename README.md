# obs-dpdfnet

An OBS Studio audio filter that removes background noise from your microphone using [DPDFNet](https://github.com/Ceva-IP/DPDFNet), a neural speech-enhancement model. It runs locally on your CPU, makes no network requests, and is tuned for a close dynamic microphone at 48 kHz.

| Platform | Availability | Requirements |
| --- | --- | --- |
| Windows (x64) | [Release zip](https://github.com/orienw/obs-dpdfnet/releases) | Windows 10/11 64-bit, OBS Studio 32.0+ |
| macOS (Apple silicon) | [Release zip](https://github.com/orienw/obs-dpdfnet/releases) | macOS 14+, OBS Studio 32.0+ |
| Linux | [Build from source](BUILDING.md#linux) | OBS Studio 30+ |

<a id="install-a-release-build"></a>
## Install

Download the release zip for your system from [GitHub Releases](https://github.com/orienw/obs-dpdfnet/releases).

### Windows

> The binary is unsigned, so SmartScreen or Defender may warn on download or first load.

1. Close OBS.
2. Extract the zip and copy the `obs-dpdfnet` folder into `%ProgramData%\obs-studio\plugins\`.
3. Verify that `%ProgramData%\obs-studio\plugins\obs-dpdfnet\bin\64bit\obs-dpdfnet.dll` exists.
4. Start OBS.

### macOS

> The plugin is not notarized, so macOS blocks it until you clear the quarantine flag in step 4.

1. Quit OBS.
2. Create the OBS plugins folder (OBS does not create it automatically) and open it in Finder:

   ```bash
   mkdir -p ~/Library/Application\ Support/obs-studio/plugins
   open ~/Library/Application\ Support/obs-studio/plugins
   ```

3. Extract the zip and drag `obs-dpdfnet.plugin` into that folder.
4. Remove the macOS quarantine attribute:

   ```bash
   xattr -dr com.apple.quarantine ~/Library/Application\ Support/obs-studio/plugins/obs-dpdfnet.plugin
   ```

5. Start OBS. If OBS reports that `obs-dpdfnet` failed to load, run step 4 again and restart OBS.

### Add the filter in OBS

Go to **Audio Mixer → mic gear → Filters → `+` → DPDFNet Noise Suppression**.

- Use the filter on a **microphone**, not desktop audio or music.
- Set OBS to **48 kHz** in **Settings → Audio** (bundled models run natively at 48 kHz; other sample rates are resampled).

<details>
<summary>Verify download checksum (SHA-256)</summary>

Compare the output against the SHA-256 shown next to the zip on the release page:

**Windows (PowerShell):**

```powershell
Get-FileHash .\obs-dpdfnet-<version>-windows-x64.zip -Algorithm SHA256
```

**macOS (Terminal):**

```bash
shasum -a 256 obs-dpdfnet-<version>-macos-arm64.zip
```

</details>

## Settings

| Setting | Description |
| --- | --- |
| **Model** | `DPDFNet8` (default) sounds best; `DPDFNet2` uses less CPU. `Custom ONNX model` loads your own file — see [Custom models](BUILDING.md#custom-models). |
| **Input channel** | Channel your microphone is on, usually `Input 1 / left`. `Mix all channels` averages them. Enhanced voice is sent to every output channel except LFE. |
| **Suppression limit** | Maximum noise removed. Start at 24–30 dB and raise only if room noise is still obvious while you speak. 40 dB is aggressive; 60 dB is a diagnostic extreme. |
| **Mix** | Blend of enhanced and original audio (`100%` = enhanced only). |
| **Output gain** | Gain after the blend. Start at `0 dB`; positive values can clip. |
| **Bypass** | Passes original audio at the same delay while keeping the model running for A/B comparison. To stop CPU use, disable the filter in OBS instead. |
| **Reset processing** | Restarts processing if paused or turned off, and clears model state, fault counters, and timing. |

## Behavior and diagnostics

- **50 ms latency:** With the bundled models, the filter delays audio by 50 ms (40 ms model + 10 ms STFT overlap). Original audio in the mix, bypass, and timestamps stay aligned so OBS keeps audio in sync with video. Audio monitoring hears the delay.
- **CPU overload fallback:** If your CPU cannot keep up in real time, the filter smoothly fades out suppression (100 ms out, 200 ms back in) while keeping audio passing at the same delay. It reports the pause in `Status` and retries automatically (after 10 s, 30 s, then every 60 s). Switch to `DPDFNet2` or reduce system load if pauses recur.
- **Processing failures:** If the model fails, or its output is far louder than its input, that audio passes through unprocessed and the filter tries again. Three failures without ten clean seconds between them disable processing until you press `Reset processing`.
- **Status line:** Shows what the filter is doing and what listeners hear. Pauses and recovery update automatically; press `Refresh` to update processing load and diagnostics.

<details>
<summary>Diagnostics details (<code>Show details</code>)</summary>

Enabling **Show details** in the **Diagnostics** group displays processing measurements for the current run (not end-to-end microphone latency):

- **Model info:** Active model, native vs. resampled execution, and frame/hop sizes.
- **Delay:** Model + STFT delay (excludes resampling and OBS buffering).
- **Callback timing:** Slowest callback and missed deadlines. Resets on model, format, resampler, or reset changes; excludes passthrough.
- **Fault counters:** Oversized packets, buffer-capacity hits, and timestamp jumps (kept until reset or model change). A timestamp jump is audio arriving ≥70 ms from where the previous packet ended (past OBS's timestamp-smoothing threshold), causing the filter to drop buffered audio and restart.

</details>

## Building from source

See [BUILDING.md](BUILDING.md) for Windows PowerShell scripts, Linux/macOS/Windows CMake builds, CMake options, tests, benchmarks, and custom ONNX model requirements.

## License

GPL-2.0-or-later for the plugin source. The bundled [DPDFNet](https://github.com/Ceva-IP/DPDFNet) models (tracked in [models/manifest.json](models/manifest.json)) and downloaded dependencies keep their upstream licenses; see [LICENSE](LICENSE), [THIRD_PARTY.md](THIRD_PARTY.md), and [LICENSES/](LICENSES/).
