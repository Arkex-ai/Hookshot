"""Complete nn_fs.c with authored guest memory: actual pack/disk handles and redirect priority, no game."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ['--cxx', '{cxx}']
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('romfs_pack', ROOT/'nx2/romfs_pack.py')
writer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(writer)

HOS = r'''
#ifndef AUTHORED_HOS_H
#define AUTHORED_HOS_H
#include <stdint.h>
#include <stddef.h>
typedef struct { uint64_t x[32]; } Ctx;
extern unsigned char authored_guest[0x10000];
extern int hos_trace;
#define HOS_SPACE_END 0x10000
#define HLE(sym) void imp_##sym(Ctx *c)
#define ARG(n) (c->x[n])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define GP(a) ((void *)(authored_guest + (uint64_t)(a)))
#define G64(a) (*(uint64_t *)GP(a))
#define G32(a) (*(uint32_t *)GP(a))
#define HOS_WRITTEN(p,n) authored_written((p),(n))
static inline int hos_guest_range(uint64_t a, uint64_t n) { return a <= HOS_SPACE_END && n <= HOS_SPACE_END - a; }
void authored_written(void *, size_t);
void hos_log(const char *, ...);
void hos_fatal(Ctx *, const char *, ...);
int hos_vcore_away(void);
void hos_vcore_back(void);
int64_t hos_now_ns(void);
void hos_sleep_ns(int64_t);
#endif
'''


def checked(cmd):
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                            creationflags=0x08000000 if os.name == 'nt' else 0)
    if result.returncode:
        raise RuntimeError((cmd, result.returncode, result.stdout, result.stderr))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='romfs-hle-') as tmp:
        folder = Path(tmp)
        source = folder/'romfs-é'
        source.mkdir()
        (source/'pattern').write_bytes(bytes((i * 17 + 3) % 251 for i in range(4096)))
        (source/'Movie').mkdir()
        (source/'Movie/a.webm').write_bytes(b'authored movie placeholder')
        (source/'Movie/Café').write_bytes(b'UNICODE')
        (source/'Movie/Mixed').mkdir()
        (source/'Movie/Mixed/Leaf').write_bytes(b'LEAF')
        (source/'empty-dir').mkdir()
        (source/'nested/deeper').mkdir(parents=True)
        (source/'nested/one').write_bytes(b'one')
        (source/'nested/deeper/file').write_bytes(b'deep')
        image = folder/'café-image.pack'
        writer.pack(source, image)
        (source/'pattern').write_bytes(b'LOOSE')
        (source/'loose-only').write_bytes(b'stale loose file')
        mod, wide = folder/'mod-file', folder/'wide-file'
        mod.write_bytes(b'MOD'); wide.write_bytes(b'WIDE')
        overlay = folder/'overlay'
        overlay.mkdir(); (overlay/'pattern').write_bytes(b'OVERLAY')
        (folder/'authored_hos.h').write_text(HOS, encoding='utf-8')
        actual = (ROOT/'host/nn_fs.c').read_text(encoding='utf-8')
        assert actual.count('#include "hos.h"') == 1
        (folder/'nn_fs.c').write_text(actual.replace('#include "hos.h"', '#include "authored_hos.h"'), encoding='utf-8')
        driver = folder/'driver.c'
        driver.write_text((ROOT/'tests/romfs_pack_fs.c').read_text(encoding='utf-8'), encoding='utf-8')
        exe = folder/('fs.exe' if os.name == 'nt' else 'fs')
        cmd = [args.cxx, '-x', 'c', '-std=c11', '-O1', '-Wall',
               '-Werror=implicit-function-declaration', '-Werror=incompatible-pointer-types',
               '-I'+str(ROOT/'host'), '-include', str(ROOT/'gpu/vk/compat/hookshot_env.h'),
               str(driver), '-o', str(exe)]
        if os.name == 'nt':
            cmd += ['-municode']
        else:
            obj = folder/'posix.o'
            checked([args.cxx, '-x', 'c', '-std=gnu11', '-O1', '-I'+str(ROOT/'host/posix'),
                     '-ffunction-sections', '-fdata-sections', '-c',
                     str(ROOT/'host/posix/win32_posix.c'), '-o', str(obj)])
            cmd += ['-D_GNU_SOURCE', '-I'+str(ROOT/'host/posix'), '-x', 'none', str(obj),
                    '-Wl,--gc-sections', '-pthread', '-ldl', '-lm']
        checked(cmd)
        result = checked([str(exe), str(image), str(source), str(mod), str(wide), str(overlay)])
        print(result.stdout.strip())
        if args.mutations:
            edits = [('loose-fallback', '!want_write && pack_path(g, path, rel)', '0'),
                     ('actual-written-count', 'done = got;', 'done = size;'),
                     ('read-only-mount', 'if (want_write && !writable)', 'if (0)'),
                     ('directory-source', 'dh->packed_directory = pack_path(gstr(ARG(1)), path, rel);', 'dh->packed_directory = 0;'),
                     ('duplicate-mod-entry', '|| romfs_pack_file_find(&romfs_image, rel)', ''),
                     ('empty-directory', '!base_exists && dh->find == INVALID_HANDLE_VALUE && !dh->nextras',
                      'dh->find == INVALID_HANDLE_VALUE && !dh->nextras')]
            if os.name == 'nt':
                edits += [('windows-file-case', 'from_pack ? romfs_pack_file_find(&romfs_image, rel)',
                           'from_pack ? romfs_pack_find(&romfs_image.pack, rel)'),
                          ('windows-directory-case', 'romfs_pack_file_dir_open(&romfs_image, rel, &dh->pack_dir)',
                           'romfs_pack_dir_open(&romfs_image.pack, rel, &dh->pack_dir)')]
            for name, old, new in edits:
                assert actual.count(old) == 1
                changed = actual.replace(old, new).replace('#include "hos.h"', '#include "authored_hos.h"')
                (folder/'nn_fs.c').write_text(changed, encoding='utf-8')
                checked(cmd)
                result = subprocess.run([str(exe), str(image), str(source), str(mod), str(wide), str(overlay)],
                                        capture_output=True, text=True, timeout=30,
                                        creationflags=0x08000000 if os.name == 'nt' else 0)
                assert result.returncode == 1, (name, result.returncode, result.stdout, result.stderr)
                print('rejected integration mutation: ' + name)


if __name__ == '__main__':
    main()
