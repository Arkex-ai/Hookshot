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
#include <cstddef>
#include "gpu/vk/vk_api.h"
using DWORD = unsigned long;
using HMODULE = void*;
constexpr unsigned MAX_PATH = 260;
static int host_screen_size(int* width, int* height) { *width=3440; *height=1440; return 1; }
// ACTUAL_HOST_INITIALIZER
static_assert(offsetof(MkwVkHost, romfs_read) == offsetof(MkwVkHost, screen_size) + sizeof(MkwVkHost::screen_size));
static HMODULE vk_module;
static int packed, init_calls;
static const MkwVkHost* received;
static const MkwVkRenderer* vk;
static unsigned provider_version = MKWVK_API_VERSION;
static unsigned created_calls;
static const nvn_prog* observed_program;
static void authored_program_created(const nvn_prog* p) { ++created_calls; observed_program=p; }
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
    static MkwVkRenderer renderer{}; renderer.version = provider_version;
    renderer.nvn_program_created = authored_program_created;
    received = api; ++init_calls; return &renderer;
}
static void* GetProcAddress(HMODULE, const char*) { return reinterpret_cast<void*>(&fake_init); }
'''
MAIN = r'''
int main() {
    nvn_prog program{};
    rx_nvn_program_created(&program);
    if (created_calls) return 5;
    for (packed = 0; packed != 2; ++packed) {
        // A stale callback must also be cleared when the folder path is selected.
        vk_host.romfs_read = hos_fs_romfs_read;
        vk = load_vulkan("authored.dll");
        if (!vk || !received || init_calls != packed + 1 ||
            vk_module != reinterpret_cast<void*>(1)) return 2;
        const auto expected = packed ? hos_fs_romfs_read : nullptr;
        int width=0, height=0;
        if (!received->screen_size || !received->screen_size(&width,&height) || width!=3440 || height!=1440) return 4;
        rx_nvn_program_created(&program);
        if (created_calls != unsigned(packed+1) || observed_program != &program) return 5;
        if (received->romfs_read != expected) {
            std::fprintf(stderr, "font source mismatch: packed=%d\n", packed); return 3;
        }
    }
    provider_version = MKWVK_API_VERSION - 1;
    if (load_vulkan("older.dll")) return 6;
    std::puts("PASS: actual loader preserves callbacks, selects folder/packed fonts and rejects an older renderer ABI");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--backend-source", type=Path, default=ROOT / "host/rx_backend.c")
    args = parser.parse_args()
    source = args.backend_source.read_text(encoding="utf-8")
    body = function(source,
                    "static const MkwVkRenderer *load_vulkan(")
    forwarder = function(source, "void rx_nvn_program_created(")
    # Compile the actual initializer's slot order. Unrelated providers are null,
    # while screen_size remains typed and must survive font selection unchanged.
    import re
    initializer = function(source, "static MkwVkHost vk_host =") + ";"
    initializer = re.sub(r"/\*.*?\*/", "", initializer, flags=re.S)
    start = initializer.index("{")
    initializer = initializer[:start] + re.sub(r"\b[A-Za-z_]\w*\b",
        lambda m: m[0] if m[0] in ("MKWVK_API_VERSION", "host_screen_size") else "nullptr",
        initializer[start:])
    prefix = PREFIX.replace("// ACTUAL_HOST_INITIALIZER", initializer)
    with tempfile.TemporaryDirectory(prefix="romfs-font-backend-") as directory:
        source = Path(directory) / "caller.cpp"
        executable = Path(directory) / ("caller.exe" if os.name == "nt" else "caller")
        source.write_text(prefix + body + forwarder + MAIN, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT), str(source), "-o", str(executable)],
                       check=True, timeout=60)
        env = os.environ.copy()
        compiler = Path(shutil.which(args.cxx) or args.cxx).resolve()
        env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([str(executable)], check=True, timeout=10, env=env)


if __name__ == "__main__":
    main()
