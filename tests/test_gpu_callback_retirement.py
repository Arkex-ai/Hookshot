"""Deferred GPU-resource callbacks wait for completion and drain without another upscale.

This compiles the real helpers, with a controlled timeline and minimal state. It
needs neither Vulkan nor game files. --source may name a pristine tree to run the
same behavioral assertions against the old implementation.
"""
RUN_ALL_ARGS = ["--cxx", "{cxx}"]

import argparse
import contextlib
import os
import re
import shutil
import time
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BACKENDS = ("vk_fsr3_inputs.cpp", "vk_reactive.cpp", "vk_temporal.cpp")
CASES = (
    "empty", "pending-age", "completed-young", "out-of-order", "idle-completed",
    "no-timeline", "overflow", "query-failure", "issued-not-completed",
    "capture-lifetime", "exact-once", "repeated", "mutex-idle",
)
TEMPORAL_CASES = ("keep-selected", "keep-cooldown", "leave-engine")



@contextlib.contextmanager
def test_directory():
    folder = Path(tempfile.mkdtemp(prefix="gpu-callback-retirement-"))
    try:
        yield folder
    finally:
        # Windows can briefly retain an image-file handle after the waited child
        # has exited. Retry only our temporary directory; never ignore a failure.
        for attempt in range(20):
            try:
                shutil.rmtree(folder)
                break
            except PermissionError:
                if attempt == 19:
                    raise
                time.sleep(0.1)


def block(source: str, marker: str) -> str:
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        if end == len(source):
            raise ValueError(f"Unclosed source block: {marker}")
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def translation_unit(source: str, replay: str, fixture: str, temporal: bool) -> str:
    timeline = "\n".join(block(replay, name) for name in (
        "uint64_t vk_retirement_value()", "uint64_t vk_timeline_completed()",
    ))
    production = "\n".join(block(source, name) for name in ("void later(", "void free_dead("))
    # The baseline has no idle entry for inputs/reactive. Model that absent call,
    # rather than fail to compile: idle-completed must fail on its behavior.
    if temporal:
        idle = block(source, "void temporal_idle()")
    elif "void collect_retired()" in source:
        idle = block(source, "void collect_retired()")
    else:
        idle = "void collect_retired() {}"
    free_call = "free_dead(false)" if "void free_dead(bool all)" in source else "free_dead()"
    return ("#define TEMPORAL_TEST\n" if temporal else "") + (
        fixture.replace("// DEAD_PRODUCTION", block(source, "struct Dead {") + ";")
        .replace("// TIMELINE_PRODUCTION", timeline)
        .replace("// RETIRE_PRODUCTION", production)
        .replace("// IDLE_PRODUCTION", idle)
        .replace("FREE_CALL", free_call)
        .replace("IDLE_CALL", "temporal_idle()" if temporal else "collect_retired()")
    )



def check_idle_wiring(replay: str, cxx: str, folder: Path, env: dict[str, str]) -> bool:
    """Execute the actual retirement statements around do_present's replay lock."""
    present = block(replay, "void do_present(")
    lock = "std::lock_guard lk(replay_mu);"
    start = present.index(lock)
    stop = present.index("PROF(PROF_PRESENT);", start)
    before = "\n".join(re.findall(
        r"(?m)^\s*((?:temporal_idle|fsr3_inputs::collect_retired|reactive::collect_retired|fsr3::collect_retired)\(\);)",
        present[:start],
    ))
    # Keep the original lock and calls, not an implementation of their expected order.
    inside = present[start:stop]
    code = r"""
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>
int presents = 0;
std::vector<int> frame_submits;
void bench_frame(int) {}
struct SlowWatch { explicit SlowWatch(const char*) {} };
bool held = false;
unsigned temporal_calls = 0, input_calls = 0, reactive_calls = 0;
void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(2); }
}
struct ReplayMutex {
    void lock() { check(!held, "recursive replay lock"); held = true; }
    void unlock() { check(held, "unbalanced replay lock"); held = false; }
} replay_mu;
void temporal_idle() { check(held, "temporal retirement outside replay lock"); ++temporal_calls; }
namespace fsr3_inputs { void collect_retired() { check(held, "inputs outside replay lock"); ++input_calls; } }
namespace reactive { void collect_retired() { check(held, "reactive outside replay lock"); ++reactive_calls; } }
namespace fsr3 { void collect_retired() { check(held, "FSR outside replay lock"); } }
void present_idle() {
// BEFORE_REPLAY_LOCK
    {
// INSIDE_REPLAY_LOCK
    }
}
int main() {
    present_idle();
    check(!held, "replay lock not released");
    check(temporal_calls == 1 && input_calls == 1 && reactive_calls == 1,
          "an idle present did not collect each callback queue exactly once");
    return 0;
}
"""
    code = code.replace("// BEFORE_REPLAY_LOCK", before).replace("// INSIDE_REPLAY_LOCK", inside)
    cpp = folder / "idle-wiring.cpp"
    exe = folder / ("idle-wiring.exe" if os.name == "nt" else "idle-wiring.test")
    cpp.write_text(code, encoding="utf-8")
    subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                    str(cpp), "-o", str(exe)], env=env, check=True, timeout=90)
    result = subprocess.run([str(exe)], env=env, capture_output=True, text=True, timeout=5)
    print("PASS idle-present-wiring" if result.returncode == 0 else
          "FAIL idle-present-wiring: " + (result.stderr or result.stdout).strip(), flush=True)
    return result.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    replay = (args.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    fixture = (ROOT / "tests/gpu_callback_retirement_test.cpp").read_text(encoding="utf-8")
    env = dict(os.environ)
    # The production idle timeout remains enabled; an operator's unrelated run
    # switch must not disable the assertions about completed retirement.
    env.pop("RX_VK_DLSS_IDLE_RELEASE", None)
    passed = failed = 0
    with test_directory() as temp:
        if check_idle_wiring(replay, args.cxx, Path(temp), env):
            passed += 1
        else:
            failed += 1
        for backend in BACKENDS:
            temporal = backend == "vk_temporal.cpp"
            source = (args.source / "gpu/vk" / backend).read_text(encoding="utf-8")
            cpp = Path(temp) / (backend + ".cpp")
            exe = Path(temp) / (backend + (".exe" if os.name == "nt" else ".test"))
            cpp.write_text(translation_unit(source, replay, fixture, temporal), encoding="utf-8")
            subprocess.run([
                args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                str(cpp), "-o", str(exe),
            ], check=True, env=env, timeout=90)
            for case in CASES + (TEMPORAL_CASES if temporal else ()):
                result = subprocess.run([str(exe), case], env=env, capture_output=True,
                                        text=True, timeout=5)
                if result.returncode:
                    failed += 1
                    print(f"FAIL {backend}/{case}: {(result.stderr or result.stdout).strip()}", flush=True)
                else:
                    passed += 1
                    print(f"PASS {backend}/{case}", flush=True)
    print(f"GPU callback retirement: {passed} passed, {failed} failed", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())