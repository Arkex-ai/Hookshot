"""Frame generation Off releases its size-dependent images only after both users finish.

Compile the real idle/destruction functions with controlled queue and device
completion. --source permits the same assertions against an unmodified tree.
No game, Vulkan installation or external Python modules are needed.
"""
RUN_ALL_ARGS = ["--cxx", "{cxx}"]

import argparse
import contextlib
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = (
    "empty", "active", "cooldown", "off-releases", "once", "pending-job",
    "busy-set", "gpu-pending", "re-enable", "frame-rewind", "rebuild",
    "null-images", "queue-mutex", "host-oom", "device-oom", "unknown-error",
    "failure-reenable", "device-lost",
)


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


@contextlib.contextmanager
def test_directory():
    folder = Path(tempfile.mkdtemp(prefix="framegen-idle-"))
    try:
        yield folder
    finally:
        # A waited Windows executable may briefly retain its image-file handle.
        # Retry only our directory and still report persistent cleanup failures.
        for attempt in range(20):
            try:
                shutil.rmtree(folder)
                break
            except PermissionError:
                if attempt == 19:
                    raise
                time.sleep(0.1)


def wiring(replay: str) -> str:
    present = block(replay, "void do_present(")
    lock = present.index("std::lock_guard lk(replay_mu);")
    record = present.index("Recording& r = begin_recording(value);", lock)
    needle = "framegen_idle();"
    positions = []
    start = 0
    while True:
        index = present.find(needle, start)
        if index < 0:
            break
        positions.append(index)
        start = index + len(needle)
    before = "\n".join(needle for index in positions if index < lock)
    inside = "\n".join(needle for index in positions if lock <= index < record)
    after = "\n".join(needle for index in positions if index >= record)
    return r"""
#include <cstdio>
#include <cstdlib>
#include <mutex>
bool held = false, recording = false;
unsigned calls = 0;
void need(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "%s\n", why); std::exit(2); }
}
struct ReplayMutex {
    void lock() { need(!held, "recursive lock"); held = true; }
    void unlock() { need(held, "unbalanced lock"); held = false; }
} replay_mu;
void framegen_idle() {
    need(held && !recording, "idle release must precede command recording under replay lock");
    ++calls;
}
int main() {
// BEFORE_LOCK
    {
        std::lock_guard lk(replay_mu);
// BEFORE_RECORD
        recording = true;
// AFTER_RECORD
    }
    need(calls == 1 && !held, "present must call the idle collector exactly once");
    return 0;
}
""".replace("// BEFORE_LOCK", before).replace("// BEFORE_RECORD", inside).replace("// AFTER_RECORD", after)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--source", type=Path, default=ROOT)
    args = parser.parse_args()
    source = (args.source / "gpu/vk/vk_framegen.cpp").read_text(encoding="utf-8")
    replay = (args.source / "gpu/vk/vk_replay.cpp").read_text(encoding="utf-8")
    fixture = (ROOT / "tests/framegen_idle_test.cpp").read_text(encoding="utf-8")
    production = "\n".join(block(source, name) for name in ("void drop_img(", "void drop_size("))
    # No idle entry on the base is a behavioral failure, not a compiler failure.
    idle = block(source, "void framegen_idle()") if "void framegen_idle()" in source else "void framegen_idle() {}"
    wait_marker = "VkResult device_wait_idle_locked()" if "VkResult device_wait_idle_locked()" in replay else "void device_wait_idle_locked()"
    wait = block(replay, wait_marker)
    code = (fixture.replace("// WAIT_PRODUCTION", wait).replace("// DROP_PRODUCTION", production)
            .replace("// IDLE_PRODUCTION", idle))
    passed = failed = 0
    with test_directory() as folder:
        for name, text, cases in (("lifetime", code, CASES), ("wiring", wiring(replay), (None,))):
            cpp = folder / (name + ".cpp")
            exe = folder / (name + (".exe" if os.name == "nt" else ".test"))
            cpp.write_text(text, encoding="utf-8")
            subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-pthread",
                            str(cpp), "-o", str(exe)], check=True, timeout=90)
            for case in cases:
                result = subprocess.run([str(exe)] + ([case] if case else []), capture_output=True,
                                        text=True, timeout=5)
                label = case or name
                if result.returncode:
                    failed += 1
                    print(f"FAIL {label}: {(result.stderr or result.stdout).strip()}", flush=True)
                else:
                    passed += 1
                    print(f"PASS {label}", flush=True)
    print(f"Frame-generation idle lifetime: {passed} passed, {failed} failed", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())