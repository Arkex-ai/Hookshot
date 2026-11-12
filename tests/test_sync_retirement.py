"""Deferred synchronization objects stay collectible until their multi-wait links are removed.

Uses the actual object table, collector and Link/Unlink calls with a fake monotonic
clock. A native two-thread regression pauses the old early decrement or the fixed final
publication, with payload quarantine on a failing control. No game or GPU is required.
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
    p.add_argument("--case", choices=["holder", "multiwait", "growth", "follower", "keep", "race", "race-after"])
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
        body = block(s, marker)
        # Expose the old early decrement's interleaving without permitting a real invalid write.
        if name == "UNLINK":
            body = body.replace("mw->links[i] = mw->links[--mw->nlinks];",
                                "test_link_write(&mw->links[i], mw->links[test_paused_decrement(&mw->nlinks)]);")
            body = body.replace("mw->links[i] = mw->links[n];",
                                "test_link_write(&mw->links[i], mw->links[n]);")
        t = t.replace("@" + name + "@", body)
    cases = [args.case] if args.case else ["holder", "multiwait", "growth", "follower", "keep", "race", "race-after"]
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
