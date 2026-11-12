"""Compile an authored ASTC GPU fixture; --run also compares the shipped shader on a selected Vulkan device.

Default CI execution only compiles the fixture. Hardware execution needs a local
Vulkan loader/device and the pinned baseline Git object (or --baseline-header).
No game files, saves, caches or encoder download are used.
"""
# SPDX-License-Identifier: GPL-3.0-or-later
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]
ROOT = Path(__file__).resolve().parents[1]
HEADER = "gpu/vk/host_shaders/astc_decoder_comp_spv.h"
BASELINE = "51da0464b4cf0c7f56b81ebd6bb309325835e175"
BASELINE_SPIRV_SHA256 = "acb9f5c0e30f7ebd3dc0c39c709a62a32a3b7724aeccce85cebdff1705e2afc4"


def spirv(header):
    body = re.search(r"ASTC_DECODER_COMP_SPV\s*\[\s*\]\s*=\s*\{(.*?)\};", header, re.S)
    if not body:
        raise RuntimeError("ASTC generated-header array was not found")
    words = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", body.group(1))]
    if not words or words[0] != 0x07230203:
        raise RuntimeError("ASTC header does not contain a SPIR-V module")
    return struct.pack("<" + "I" * len(words), *words)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--run", action="store_true", help="execute Vulkan work; default only compiles")
    parser.add_argument("--device", default="", help="case-insensitive device-name substring; empty selects hardware")
    parser.add_argument("--baseline-header", type=Path, help="GPL baseline header for a checkout without the pinned Git object")
    parser.add_argument("--candidate-spirv", type=Path, help="explicit shader override for semantic negative controls")
    args = parser.parse_args()
    compiler = shutil.which(args.cxx) or (str(args.cxx) if Path(args.cxx).is_file() else None)
    if not compiler:
        raise RuntimeError("C++ compiler unavailable: " + args.cxx)
    if not (args.headers / "vulkan/vulkan_core.h").is_file():
        raise RuntimeError("Vulkan core headers unavailable: " + str(args.headers))
    env = os.environ.copy()
    if os.name == "nt":
        env["PATH"] = str(Path(compiler).resolve().parent) + os.pathsep + env.get("PATH", "")
    with tempfile.TemporaryDirectory(prefix="hookshot-astc-gpu-") as directory:
        directory = Path(directory)
        executable = directory / ("fixture.exe" if os.name == "nt" else "fixture")
        flags = ["-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers"]
        libraries = [] if os.name == "nt" else ["-ldl"]
        subprocess.run([compiler, *flags, "-I" + str(args.headers), str(ROOT / "tests/astc_decoder_gpu.cpp"),
                        "-o", str(executable), *libraries], check=True, timeout=120, env=env)
        if not args.run:
            print("PASS: ASTC GPU fixture compiled; no Vulkan loader/device work executed (use --run)")
            return
        if args.baseline_header:
            baseline_header = args.baseline_header.read_text(encoding="utf-8")
        else:
            reference = subprocess.run(["git", "-C", str(ROOT), "show", BASELINE + ":" + HEADER],
                                       capture_output=True, text=True, encoding="utf-8", timeout=30)
            if reference.returncode:
                raise RuntimeError("Pinned GPL baseline Git object unavailable; fetch " + BASELINE +
                                   " or supply --baseline-header. No automatic fetch is performed.")
            baseline_header = reference.stdout
        baseline = spirv(baseline_header)
        if hashlib.sha256(baseline).hexdigest() != BASELINE_SPIRV_SHA256:
            raise RuntimeError("Baseline shader differs from the pinned Vulkan 1.1 module")
        candidate = args.candidate_spirv.read_bytes() if args.candidate_spirv else spirv(
            (ROOT / HEADER).read_text(encoding="utf-8"))
        first, second = directory / "baseline.spv", directory / "candidate.spv"
        first.write_bytes(baseline)
        second.write_bytes(candidate)
        command = [str(executable), str(first), str(second)]
        if args.device:
            command.append(args.device)
        result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        if result.returncode == 77:
            raise RuntimeError("Explicit --run could not execute a compatible Vulkan device; this is not a GPU pass")
        if result.returncode:
            raise RuntimeError("ASTC GPU fixture failed with exit " + str(result.returncode))
        if "PASS full ASTC RGBA8 GPU equality: 88 cases" not in result.stdout:
            raise RuntimeError("Fixture did not report the complete 88-case comparison")
        print("Baseline SPIR-V SHA-256:", hashlib.sha256(baseline).hexdigest())
        print("Candidate SPIR-V SHA-256:", hashlib.sha256(candidate).hexdigest())


if __name__ == "__main__":
    main()
