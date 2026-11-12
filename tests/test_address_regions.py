"""Address regions are not reused or exposed while their old pages are being discarded.

Compile the actual allocator/readers from hos_mem.c with controlled commit/discard and
native threads. Covers reuse ordering, callback reentry, table relocation, live-range
queries, reclaimed heap bounds and empty/repeated finalization. No game or GPU required.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]
MARKERS = ['uint64_t hos_region_alloc(', 'static void pages_mark_used_locked(', 'void hos_region_free(', 'int hos_heap_size_fits(', 'uint64_t hos_region_floor(', 'int hos_mem_region(', 'int hos_regions(']


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "clang" if os.name == "nt" else "cc"))
    parser.add_argument("--source", type=Path, default=ROOT / "host" / "hos_mem.c")
    args = parser.parse_args()
    cc = shutil.which(args.cc)
    if not cc:
        parser.error("C compiler not found: " + args.cc)
    source = args.source.read_text(encoding="utf-8")
    declaration = re.search(r"typedef struct \{[^}]*\} Range;", source)
    if not declaration:
        raise AssertionError("Range declaration not found")
    text = (ROOT / "tests" / "address_regions_test.c").read_text(encoding="utf-8")
    text = text.replace("@RANGE@", declaration.group(0))
    for index, marker in enumerate(MARKERS):
        text = text.replace("@FUNCTION_" + str(index) + "@", block(source, marker))
    with tempfile.TemporaryDirectory(prefix="hookshot-address-regions-") as tmp:
        path = Path(tmp)
        c = path / "check.c"
        exe = path / ("check.exe" if os.name == "nt" else "check")
        c.write_text(text, encoding="utf-8")
        command = [cc, "-x", "c", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)]
        if os.name != "nt":
            command.append("-pthread")
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == "__main__":
    main()
