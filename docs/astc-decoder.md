# ASTC decoder invariants and regression checks

The compute decoder calculates partition hash/scales and the weight-grid scale once
per ASTC block, then reuses them for its pixels. Endpoint encoding uses a boundary
search with the original tie rule. Descriptor bindings, push constants, workgroup
size, tiling, HDR branches and the upload/BC conversion policy are unchanged.

The optimization follows [Lichii's decoder change](https://github.com/zortos293/Lichii/commit/d590d51820f99447bd7056920e28eaeab510e4f1),
including justr4mzi/SmileyTatsu's work. The existing Eden/yuzu GPL notices remain in
the shader; the frozen test reference carries its own notices. This is relative
output preservation, not a new independent ASTC conformance implementation.

## Game-free checks

`tests/test_astc_decoder.py` compiles the actual partition and endpoint logic
against frozen pre-change GPL logic. It compares 3,935,232 partition positions and
8,448 endpoint choices. This covers those domains, not every ASTC decoding mode.

`tests/test_astc_decoder_gpu.py` compiles a standalone Vulkan fixture in the normal
source suite. It **does not execute GPU work by default**. To opt in locally:

```sh
python -I -B tests/test_astc_decoder_gpu.py --cxx clang++ --headers /path/to/vulkan/include --run
```

On Windows, pass llvm-mingw's `clang++.exe`. `--device NVIDIA` selects a device by
name; with no name, CPU devices are excluded. Explicit `--device llvmpipe` is a
software Vulkan check, not a hardware result. A missing compatible device is a
failure of an explicitly requested run, never a GPU pass.

The fixture compares the shipped generated shader against the header at
`51da0464b4cf0c7f56b81ebd6bb309325835e175`. Fetch that Git object first if absent,
or supply its GPL header with `--baseline-header`; the pinned binary SHA-256 is
checked. No automatic fetch, encoder download or game input is used. Authored
blocks exercise 88 cases: 22 footprints/orientations, two input layouts, and full
or clipped edges. Checks include a known-color valid void-extent block, exact
RGBA8 equality and readback padding. `--candidate-spirv` supports intentional
wrong-output controls. The fixture does not measure performance.

Windows RTX 5070 Ti and Linux llvmpipe passed all 88 cases. Wrong-zero-store and
wrong-partition-tie shader controls failed on actual NVIDIA pixel differences.
Khronos validation was unavailable. Random blocks include invalid encodings;
agreement with the baseline does not establish independent ASTC conformance.
Separate authored valid-block comparisons against Arm's reference still differ
identically on baseline and candidate; that pre-existing discrepancy is not fixed.

## Native measurement scope

[Recorded runs](tested-configurations.md) use source `51da0464` versus `3d7c7a22`,
with identical private GPU timestamp instrumentation in both renderers and the
same host, checkpoint, options and cache seed. The timestamp span covers the
actual ASTC upload commands, including barriers, BC conversion and copies.
Matching uses staging-input snapshot fingerprints, shape/format/BC flag and
dispatch/byte counts; unmatched groups are excluded and retained in local evidence.

Three AB/BA/AB pairs reduce matched upload GPU time by 2.44%, 3.29% and 3.66%
(median 3.29%, 5.46-8.17 ms saved over approximately 223 ms of upload GPU work).
The A/A control differs by 0.26%. Matched time covers 100% of baseline and at least
99.9976% of candidate upload time. GPU clocks were dynamic. Eight processes exited
normally with the source checkpoint unchanged; an additional uninstrumented
baseline/candidate pair loaded and rendered early Kokiri normally.

These are Windows/NVIDIA early-load measurements, not FPS, uninstrumented startup,
CPU time, campaign, HDR conformance, Linux hardware, AMD or Intel results. The
private profiler is not part of this patch. Other Tier 1 hardware remains to test.
