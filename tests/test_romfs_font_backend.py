"""Exercise the actual renderer loader's font-source selection with a controlled DLL provider.

Folder installs must retain the legacy Reader path; only an open packed image
selects the bounded virtual read callback. No DLL, Vulkan device or game is used.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def function(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PREFIX = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "gpu/vk/vk_api.h"
using DWORD = unsigned long;
using HMODULE = void*;
constexpr unsigned MAX_PATH = 260;
static MkwVkHost vk_host{};
static HMODULE vk_module;
static int packed, init_calls;
static const MkwVkHost* received;
[[maybe_unused]] static int hos_fs_packed() { return packed; }
static int hos_fs_romfs_read(const char*, uint64_t, void*, uint64_t, uint64_t*) { return 1; }
static void hos_log(const char*, ...) {}
static DWORD GetModuleFileNameA(void*, char* out, unsigned) {
    const char path[] = "C:/authored/hookshot.exe";
    std::memcpy(out, path, sizeof path); return sizeof path - 1;
}
static HMODULE LoadLibraryA(const char*) { return reinterpret_cast<void*>(1); }
static DWORD GetLastError() { return 0; }
static const MkwVkRenderer* fake_init(const MkwVkHost* api) {
    static MkwVkRenderer renderer{}; renderer.version = MKWVK_API_VERSION;
    received = api; ++init_calls; return &renderer;
}
static void* GetProcAddress(HMODULE, const char*) { return reinterpret_cast<void*>(&fake_init); }
'''
MAIN = r'''
int main() {
    for (packed = 0; packed != 2; ++packed) {
        // A stale callback must also be cleared when the folder path is selected.
        vk_host.romfs_read = hos_fs_romfs_read;
        if (!load_vulkan("authored.dll") || !received || init_calls != packed + 1 ||
            vk_module != reinterpret_cast<void*>(1)) return 2;
        const auto expected = packed ? hos_fs_romfs_read : nullptr;
        if (received->romfs_read != expected) {
            std::fprintf(stderr, "font source mismatch: packed=%d\n", packed); return 3;
        }
    }
    std::puts("PASS: actual loader selects legacy folder fonts and packed virtual fonts");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    args = parser.parse_args()
    body = function((ROOT / "host/rx_backend.c").read_text(encoding="utf-8"),
                    "static const MkwVkRenderer *load_vulkan(")
    with tempfile.TemporaryDirectory(prefix="romfs-font-backend-") as directory:
        source = Path(directory) / "caller.cpp"
        executable = Path(directory) / ("caller.exe" if os.name == "nt" else "caller")
        source.write_text(PREFIX + body + MAIN, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT), str(source), "-o", str(executable)],
                       check=True, timeout=60)
        env = os.environ.copy()
        compiler = Path(shutil.which(args.cxx) or args.cxx).resolve()
        env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([str(executable)], check=True, timeout=10, env=env)


if __name__ == "__main__":
    main()
