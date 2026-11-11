"""Linux startup selection: an accessible device with hidden sysfs keeps the renderer's fallback available.

Only the production selection block executes; device/file boundaries are authored. No device, mapping or game is used.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
RUN_ALL_ARGS = ["--cc", "{cc}"]
RUN_ALL_SKIP = {"windows": "Linux memfd/sysfs startup policy"}

CASES = [
    ("positive", 42, "64", "unset", (1, 64, 1, 1, 1, 0)),
    ("sysfs newline", 42, "64\n", "unset", (1, 64, 1, 1, 1, 0)),
    ("small limit", 42, "1", "unset", (1, 1, 1, 1, 1, 0)),
    ("custom limit", 42, "128", "unset", (1, 128, 1, 1, 1, 0)),
    ("hidden sysfs", 42, "missing", "unset", (1, 64, 1, 1, 1, 1)),
    ("disabled", 42, "0", "unset", (0, 0, 0, 1, 1, 0)),
    ("malformed", 42, "bad", "unset", (0, 0, 0, 1, 1, 0)),
    ("empty readable file", 42, "", "unset", (0, 0, 0, 1, 1, 0)),
    ("device unavailable", -1, "64", "unset", (0, 0, 0, 0, 0, 0)),
    ("explicit off", 42, "64", "0", (1, 64, 0, 1, 1, 0)),
    ("explicit off with hidden sysfs", 42, "missing", "0", (1, 64, 0, 1, 1, 0)),
    ("explicit on with hidden sysfs", 42, "missing", "1", (1, 64, 1, 1, 1, 0)),
    ("explicit on with zero", 42, "0", "1", (0, 0, 1, 1, 1, 0)),
    ("explicit on without device", -1, "missing", "1", (0, 0, 1, 0, 0, 0)),
    ("explicit off without device", -1, "64", "0", (0, 0, 0, 0, 0, 0)),
    ("explicit on with malformed limit", 42, "bad", "1", (0, 0, 1, 1, 1, 0)),
]

FIXTURE = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *authored_limit;
static int reads, closes, notices;
static FILE *limit_file(const char *path, const char *mode) {
    assert(strcmp(path,"/sys/module/udmabuf/parameters/size_limit_mb")==0 && strcmp(mode,"r")==0);
    ++reads;
    if (!authored_limit) { errno=ENOENT; return NULL; }
    FILE *f=tmpfile(); assert(f); fputs(authored_limit,f); rewind(f); return f;
}
static int close_device(int fd) { assert(fd==42); ++closes; return 0; }
#define fopen limit_file
#define close close_device
#define hos_log(...) (++notices)
static void probe(int u, const char *e) {
    int usable=0;
    unsigned long long limit_mb=0;
@@BLOCK@@
    printf("%d,%llu,%d,%d,%d,%d\n",usable,limit_mb,@@SELECTION@@,reads,closes,notices);
}
int main(int argc, char **argv) {
    assert(argc==4); authored_limit=strcmp(argv[2],"missing") ? argv[2] : NULL;
    probe(atoi(argv[1]),strcmp(argv[3],"unset") ? argv[3] : NULL); return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--source", type=Path, default=ROOT / "host/hos_main.c")
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    start = source.index('            if (u >= 0) {', source.index('getenv("HOS_GUEST_MEMFD")'))
    block = source[start:source.index('            if (!usable && !e)', start)]
    selection = re.search(r'if \((e \? atoi\(e\) != 0 : usable)\)', source).group(1)
    code = FIXTURE.replace("@@BLOCK@@", block).replace("@@SELECTION@@", selection)
    with tempfile.TemporaryDirectory(prefix="udmabuf-selection-") as folder:
        root = Path(folder)

        def check(text):
            c, exe = root / "fixture.c", root / "fixture"
            c.write_text(text, encoding="utf-8")
            subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)],
                           check=True, timeout=60)
            failures = []
            for label, device, limit, override, expected in CASES:
                result = subprocess.run([str(exe), str(device), limit, override], check=True,
                                        capture_output=True, text=True, timeout=10)
                actual = tuple(map(int, result.stdout.strip().split(",")))
                if actual != expected:
                    failures.append((label, expected, actual))
            return failures

        assert not (failures := check(code)), failures
        print("PASS: 16 production startup-selection cases; no real device or process setting changed")
        if args.mutations:
            mutations = [
                ("fallback lost", "limit_mb = 64;", "limit_mb = 0;"),
                ("known zero accepted", "usable = limit_mb > 0;", "usable = 1;"),
                ("malformed accepted", 'if (fscanf(f, "%llu", &limit_mb) != 1) limit_mb = 0;',
                 'if (fscanf(f, "%llu", &limit_mb) != 1) limit_mb = 64;'),
                ("explicit off ignored", selection, "e ? usable : usable"),
                ("explicit on ignored", selection, "e ? 0 : usable"),
            ]
            for label, before, after in mutations:
                assert code.count(before) == 1, label
                assert check(code.replace(before, after)), "Undetected mutation: " + label
                print("PASS: rejected semantic mutation: " + label)


if __name__ == "__main__":
    main()
