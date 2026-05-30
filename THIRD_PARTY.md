# Third Party Components

This repository contains plugin source code plus DPDFNet model artifacts. Build
helper scripts download additional dependencies into the ignored `third_party/`
directory.

## Bundled Artifacts

- DPDFNet ONNX models from `Ceva-IP/DPDFNet`, Apache-2.0.
  - Source: `https://huggingface.co/Ceva-IP/DPDFNet`
  - Bundled files:
    - `models/dpdfnet8_48khz_hr.onnx`
    - `models/dpdfnet2_48khz_hr.onnx`
  - Source revision, sizes, and SHA-256 hashes are recorded in
    `models/manifest.json`.
  - License text: `LICENSES/Apache-2.0.txt`

## Downloaded Dependencies

- OBS Studio development headers and runtime APIs, GPL-2.0-or-later.
  - Source: `https://github.com/obsproject/obs-studio`
  - License text: `LICENSE`
- ONNX Runtime, MIT.
  - Source: `https://github.com/microsoft/onnxruntime`
  - License text: `LICENSES/MIT-ONNXRuntime.txt`
- KissFFT, BSD-3-Clause.
  - Source: `https://github.com/mborgerding/kissfft`
  - License texts: `LICENSES/BSD-3-Clause-KissFFT.txt` and
    `LICENSES/BSD-3-Clause.txt`
- SIMDe headers, MIT.
  - Source: `https://github.com/simd-everywhere/simde`
  - License text: `LICENSES/MIT-SIMDe.txt`

The direct MSVC build copies ONNX Runtime as `onnxruntime_dpdfnet.dll` to avoid
colliding with older `onnxruntime.dll` copies that may be present elsewhere on a
Windows system.

If you publish binary release packages that include ONNX Runtime DLLs, include
ONNX Runtime's `ThirdPartyNotices.txt` from the downloaded package alongside the
binary bundle.
