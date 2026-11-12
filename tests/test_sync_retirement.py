"""Deferred synchronization objects stay collectible until their multi-wait links are removed.

Uses the actual object table, collector and Link/Unlink calls with a fake monotonic
clock. No game, GPU, sleep-based timing or generated source is required.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
RUN_ALL_ARGS = ["--cc", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    a = text.index(marker)
    b = text.index("{", a) + 1
    depth = 1
    while depth:
        depth += (text[b] == "{") - (text[b] == "}")
        b += 1
    return text[a:b]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default=os.environ.get("CC", "clang" if os.name == "nt" else "cc"))
    p.add_argument("--source", type=Path, default=ROOT / "host" / "nn_os_sync.c")
    p.add_argument("--case", choices=["holder", "multiwait", "growth", "follower", "keep"])
    args = p.parse_args()
    cc = shutil.which(args.cc)
    if not cc:
        p.error("C compiler not found: " + args.cc)
    s = args.source.read_text(encoding="utf-8")
    table = s[s.index("enum { K_MUTEX"):s.index("/* Wait on o->cv")]
    # This forward declaration belongs to the omitted wait implementation, not the table.
    table = table.replace("static int wake_lat_on(void);   /* (HOS_WAKE_LAT, below) */", "")
    t = (ROOT / "tests" / "sync_retirement_test.c").read_text(encoding="utf-8")
    t = t.replace("@TABLE@", table)
    for name, marker in [("HOLDER_WATCH", "static uint64_t holder_watch("),
                         ("LINK", "HLE(_ZN2nn2os19LinkMultiWaitHolder"),
                         ("UNLINK", "HLE(_ZN2nn2os21UnlinkMultiWaitHolder"),
                         ("UNLINK_ALL", "HLE(_ZN2nn2os24UnlinkAllMultiWaitHolder")]:
        t = t.replace("@" + name + "@", block(s, marker))
    cases = [args.case] if args.case else ["holder", "multiwait", "growth", "follower", "keep"]
    with tempfile.TemporaryDirectory(prefix="hookshot-sync-retirement-") as tmp:
        c = Path(tmp) / "check.c"
        exe = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
        c.write_text(t, encoding="utf-8")
        command = [cc, "-x", "c", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)]
        if os.name != "nt":
            command.append("-pthread")
        subprocess.run(command, check=True, timeout=60)
        for case in cases:
            subprocess.run([str(exe), case], check=True, timeout=30)


if __name__ == "__main__":
    main()
