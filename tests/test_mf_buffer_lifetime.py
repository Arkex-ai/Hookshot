"""MF input/output buffer references and locks are balanced on rejected and failed paths.

Compiles the actual pull/decode bodies with deterministic HRESULT-injected interfaces.
No Windows codec, GPU or artificial memory exhaustion is required for these checks.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
RUN_ALL_ARGS = ["--cc", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
CASES = ["output-2d", "output-linear", "output-fallback", "output-negative", "output-zero", "output-null", "output-both-fail", "output-own-add-fail", "input-ok", "input-lock-fail", "input-null", "input-length-fail", "input-add-fail", "input-sample-fail", "input-buffer-fail"]


def block(s, marker):
    a = s.index(marker)
    b = s.index("{", a) + 1
    level = 1
    while level:
        level += (s[b] == "{") - (s[b] == "}")
        b += 1
    return s[a:b]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default=os.environ.get("CC", "clang" if os.name == "nt" else "cc"))
    p.add_argument("--source", type=Path, default=ROOT / "host" / "mf_av1.c")
    p.add_argument("--case", choices=CASES)
    a = p.parse_args()
    cc = shutil.which(a.cc)
    if not cc:
        p.error("C compiler not found: " + a.cc)
    s = a.source.read_text(encoding="utf-8")
    types = s[s.index("/* a picture:"):s.index("static int com_ready(")]
    t = (ROOT / "tests" / "mf_buffer_lifetime_test.c").read_text(encoding="utf-8")
    t = t.replace("@TYPES@", types).replace("@PULL@", block(s, "static void pull(")).replace("@DECODE@", block(s, "void mf_av1_decode("))
    with tempfile.TemporaryDirectory(prefix="hookshot-mf-lifetime-") as tmp:
        c = Path(tmp) / "check.c"
        exe = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
        c.write_text(t, encoding="utf-8")
        subprocess.run([cc, "-x", "c", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)], check=True, timeout=60)
        for case in [a.case] if a.case else CASES:
            subprocess.run([str(exe), case], check=True, timeout=30)


if __name__ == "__main__":
    main()
