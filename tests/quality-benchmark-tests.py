# SPDX-License-Identifier: GPL-2.0-or-later

from pathlib import Path
import math
import struct
import subprocess
import sys
import tempfile


def chunk(name, payload):
    return name + struct.pack("<I", len(payload)) + payload + b"\0" * (len(payload) % 2)


def wav(payload):
    return b"RIFF" + struct.pack("<I", len(payload) + 4) + b"WAVE" + payload


def main():
    executable = Path(sys.argv[1]).resolve()
    fixtures = Path(sys.argv[2]).resolve()
    model = fixtures / "valid_identity.onnx"
    pcm_format = chunk(b"fmt ", struct.pack("<HHIIHH", 1, 1, 48000, 96000, 2, 16))
    float_format = chunk(b"fmt ", struct.pack("<HHIIHH", 3, 1, 48000, 192000, 4, 32))
    pcm_audio = struct.pack("<hh", 8192, -8192) * 2400
    float_audio = struct.pack("<ff", 0.2, -0.1) * 2400
    valid_pcm = wav(pcm_format + chunk(b"data", pcm_audio))
    valid_float = wav(chunk(b"JUNK", b"odd") + float_format + chunk(b"data", float_audio))

    with tempfile.TemporaryDirectory(prefix="dpdfnet-quality-test-") as directory:
        root = Path(directory)
        clean = root / "clean.wav"
        noise = root / "noise.wav"
        clean.write_bytes(valid_pcm)
        noise.write_bytes(valid_float)

        def run(name, clean_path, noise_path, error=None, model_path=model, frames=4800):
            output = root / name
            result = subprocess.run(
                [str(executable), str(model_path), str(clean_path), str(noise_path), "5", str(output)],
                capture_output=True, text=True, timeout=30,
            )
            if error is None:
                assert result.returncode == 0, result.stdout + result.stderr
                assert f"frames={frames}\n" in result.stdout, result.stdout
                reference = (output / "clean-reference.wav").read_bytes()
                assert len(reference) == 44 + frames * 4
            else:
                assert result.returncode == 1 and error in result.stderr, (
                    f"{name}: {result.returncode}\n{result.stdout}{result.stderr}"
                )
                assert not output.exists(), f"{name}: malformed input produced results"
            return output

        run("valid-pcm-and-float", clean, noise)
        run("valid-float-and-pcm", noise, clean)

        tone = [0.25 * math.sin(2 * math.pi * 440 * frame / 48000) for frame in range(48000)]
        tone_path = root / "tone.wav"
        tone_path.write_bytes(wav(float_format + chunk(b"data", struct.pack("<48000f", *tone))))
        output = run(
            "custom-rate", tone_path, tone_path,
            model_path=fixtures / "valid_extreme_capacity.onnx", frames=48000,
        )
        enhanced = struct.unpack("<48000f", (output / "enhanced-clean.wav").read_bytes()[44:])
        # Check steady audio after this fixture's unusually long STFT startup.
        error = sum((tone[i] - enhanced[i]) ** 2 for i in range(30000, 47000))
        signal = sum(tone[i] ** 2 for i in range(30000, 47000))
        assert error / signal < 1e-6, "custom-rate output is not aligned with its reference"

        malformed = {
            "truncated-header": (valid_pcm[:10], "truncated WAV file"),
            "truncated-data": (valid_pcm[:-20], "invalid WAV RIFF size"),
            "short-format": (
                wav(chunk(b"fmt ", bytes(8)) + chunk(b"data", pcm_audio)),
                "WAV format chunk is too short",
            ),
            "truncated-chunk-header": (
                wav(pcm_format + b"dat"), "truncated WAV chunk header",
            ),
            "oversized-chunk": (
                wav(pcm_format + b"data" + struct.pack("<I", 1024) + pcm_audio[:2]),
                "WAV chunk exceeds RIFF size",
            ),
            "partial-pcm-sample": (
                wav(pcm_format + chunk(b"data", bytes(3))),
                "partial PCM16 sample",
            ),
            "partial-float-sample": (
                wav(float_format + chunk(b"data", bytes(6))),
                "partial float32 sample",
            ),
        }
        for name, (payload, error) in malformed.items():
            invalid = root / f"{name}.wav"
            invalid.write_bytes(payload)
            run(name, invalid, noise, error)
        run("truncated-noise", clean, root / "truncated-data.wav", "invalid WAV RIFF size")

    print("[PASS] quality benchmark WAV validation")


if __name__ == "__main__":
    main()
