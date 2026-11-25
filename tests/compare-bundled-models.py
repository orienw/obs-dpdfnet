# SPDX-License-Identifier: GPL-2.0-or-later
"""Release qualification against the original models using the native processor."""

import argparse
import array
import filecmp
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import urllib.request
import wave


ROOT = Path(__file__).resolve().parents[1]
SPEECH_URL = (
    "https://huggingface.co/datasets/Ceva-IP/DPDFNet_EvalSet/resolve/"
    "24866d8ae065f0518aef0f5f0c6200f31166af98/Noisy/"
    "arabic_airport_snr0_rt60none_0_mixture_noisy.wav"
)
SPEECH_SHA256 = "aa144f4ada2f02b1fc0c570114ffbc5711d34bc4ea7ad9a2685b6bd7fc60de47"


def check_hash(path, expected):
    if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
        raise ValueError(f"SHA-256 mismatch: {path}")


def fetch(path, url, expected):
    if not path.exists():
        with urllib.request.urlopen(url, timeout=90) as response:
            data = response.read()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f"SHA-256 mismatch: {url}")
        path.write_bytes(data)
    else:
        check_hash(path, expected)


def prepare_audio(wav, output):
    with wave.open(str(wav)) as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (1, 2, 16000):
            raise ValueError("Expected mono 16 kHz PCM16")
        samples = array.array("h", source.readframes(source.getnframes()))
    if sys.byteorder != "little":
        samples.byteswap()
    # Interpolate the pinned 16 kHz recording onto the models' 48 kHz timeline.
    audio = array.array("f")
    for left, right in zip(samples, samples[1:]):
        audio.extend((left / 32768, (2 * left + right) / 98304, (left + 2 * right) / 98304))
    with output.open("wb") as stream:
        audio.tofile(stream)
    return len(audio)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bin", type=Path)
    parser.add_argument("work", type=Path)
    parser.add_argument("--regenerate", action="store_true", help="also verify the ONNX export")
    args = parser.parse_args()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    executable_suffix = ".exe" if os.name == "nt" else ""
    dump = args.bin.resolve() / ("dpdfnet-stream-dump" + executable_suffix)
    bench = args.bin.resolve() / ("dpdfnet-processor-benchmark" + executable_suffix)
    manifest = json.loads((ROOT / "models/manifest.json").read_text())
    wav = work / "speech.wav"
    fetch(wav, SPEECH_URL, SPEECH_SHA256)
    audio = work / "input.f32"
    frames = prepare_audio(wav, audio)
    for model in manifest["models"]:
        name = model["name"]
        original = work / (name + ".onnx")
        shipped = ROOT / "models" / original.name
        url = f"https://huggingface.co/{manifest['source']}/resolve/{manifest['revision']}/{model['file']}"
        fetch(original, url, model["upstreamSha256"])
        check_hash(shipped, model["sha256"])
        if args.regenerate:
            rebuilt = work / "rebuilt.onnx"
            subprocess.run([sys.executable, ROOT / "scripts/optimize-model.py", original, rebuilt], check=True)
            check_hash(rebuilt, model["sha256"])

        for label, path in (("original", original), ("shipped", shipped)):
            pcm = work / f"{label}.f32"
            state = work / f"{label}.state"
            subprocess.run([dump, path, audio, pcm, state], check=True)
            if pcm.stat().st_size != frames * 4 or state.stat().st_size == 0:
                raise ValueError(f"Incomplete stream dump for {name} ({label})")
        for suffix in ("f32", "state"):
            if not filecmp.cmp(work / f"original.{suffix}", work / f"shipped.{suffix}", shallow=False):
                raise ValueError(f"{name}: {suffix} differs from the original")
        print(f"{name}: {frames} identical audio samples and matching recurrent-state snapshots", flush=True)

        # Reverse the order for the repeat. Report timing for review on each host.
        for label, path in (("original", original), ("shipped", shipped),
                            ("shipped", shipped), ("original", original)):
            report = subprocess.check_output([bench, path, "48000"], text=True)
            fields = dict(line.split("=", 1) for line in report.splitlines() if "=" in line)
            print(f"{name} {label}: p50={fields['p50_us']} us, p99={fields['p99_us']} us, "
                  f"missed={fields['deadline_misses']}/{fields['measured_packets']}", flush=True)


if __name__ == "__main__":
    main()
