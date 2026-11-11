"""Actual startup argument loop and pack selection: no guest, SDL, network or real installation."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ['--cxx', '{cxx}']
ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


PREFIX = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
const char *hos_romfs_dir, *hos_user_dir;
static int calls, hos_trace;
static const char *opened, *image, *romfs_pack;
int hos_fs_pack_open(const char *path) { ++calls; opened = path; return strcmp(path, "bad") != 0; }
int rx_prepare_shaders(void) { return 1; }
@SELECT@
static int arguments(int argc, char **argv) {
@PARSER@
    return select_romfs_pack(romfs_pack) ? 0 : 1;
}
int main(int argc, char **argv) {
    int result = arguments(argc, argv);
    if (hos_trace) puts("trace enabled");
    printf("calls=%d\nimage=%s\nroot=%s\nopened=%s\n", calls, image ? image : "", hos_romfs_dir ? hos_romfs_dir : "", opened ? opened : "");
    return result;
}
'''


def exercise(exe, browser):
    cases = [([], 0, 0, '', '', ''),
             (['image.bin'], 0, 0, 'image.bin', '', ''),
             (['image.bin', '--trace'], 0, 0, 'image.bin', '', ''),
             (['--romfs', 'folder', 'image.bin'], 0, 0, 'image.bin', 'folder', ''),
             (['--romfs-pack', 'good', 'image.bin'], 1 if browser else 0, 0 if browser else 1,
              'image.bin', '' if browser else 'romfs', '' if browser else 'good'),
             (['image.bin', '--romfs', 'folder', '--romfs-pack', 'good'], 1 if browser else 0,
              0 if browser else 1, 'image.bin', 'folder', '' if browser else 'good'),
             (['--romfs-pack', 'bad', 'image.bin'], 1, 0 if browser else 1,
              'image.bin', '' if browser else 'romfs', '' if browser else 'bad'),
             (['--romfs-pack'], 2, 0, '', '', ''),
             (['--romfs-pack', ''], 2, 0, '', '', ''),
             (['--romfs-pack', '--trace'], 2, 0, '', '', ''),
             (['--romfs-pack', 'good'], 1 if browser else 0, 0 if browser else 1,
              '', '' if browser else 'romfs', '' if browser else 'good'),
             (['--romfs-pack', 'bad', '--prepare-shaders'], 0, 0, '', '', '')]
    for arguments, code, calls, image, root, opened in cases:
        result = subprocess.run([str(exe), *arguments], capture_output=True, text=True, timeout=15,
                                creationflags=0x08000000 if os.name == 'nt' else 0)
        expected = f'calls={calls}\nimage={image}\nroot={root}\nopened={opened}\n'
        assert result.returncode == code and result.stdout.endswith(expected), (arguments, result.returncode, result.stdout, result.stderr)
        if code == 2:
            assert 'needs a file path' in result.stderr
        if code == 1:
            assert ('desktop builds only' if browser else 'cannot open the selected RomFS pack') in result.stderr
        if code == 0 and '--trace' in arguments:
            assert 'trace enabled' in result.stdout
    return len(cases)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default='clang++')
    p.add_argument('--mutations', action='store_true')
    args = p.parse_args()
    text = (ROOT/'host/hos_main.c').read_text(encoding='utf-8')
    select = block(text, 'static int select_romfs_pack(')
    start = text.index('const char *image = NULL, *romfs_pack = NULL;')
    parser = block(text[start:], 'for (int i = 1; i < argc; i++) {')
    assert text.index('if (!select_romfs_pack(romfs_pack)) return 1;', start) < text.index('if (!reserve_guest())', start)
    assert '!strcmp(argv[i], "--romfs-pack") || !strcmp(argv[i], "--user")' in text
    with tempfile.TemporaryDirectory(prefix='romfs-start-') as tmp:
        folder = Path(tmp)
        def build(selection, parsing, browser=False):
            source = folder/'fixture.c'
            source.write_text(PREFIX.replace('@SELECT@', selection).replace('@PARSER@', parsing), encoding='utf-8')
            exe = folder/('start.exe' if os.name == 'nt' else 'start')
            cmd = [args.cxx, '-x', 'c', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(exe)]
            if browser:
                cmd += ['-DA64_MEM_ZERO']
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                                    creationflags=0x08000000 if os.name == 'nt' else 0)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            return exe
        count = exercise(build(select, parser), False)
        count += exercise(build(select, parser, True), True)
        print(f'{count} startup cases passed (desktop and browser refusal)')
        if args.mutations:
            assert select.count('        return 0;') == 1 and parser.count('romfs_pack = argv[++i];') == 1
            for name, selection, parsing in [
                ('invalid-pack-continues', select.replace('        return 0;', '        return 1;'), parser),
                ('pack-path-not-consumed', select, parser.replace('romfs_pack = argv[++i];', 'romfs_pack = argv[i];'))]:
                try:
                    exercise(build(selection, parsing), False)
                except AssertionError:
                    print('rejected startup mutation: '+name)
                else:
                    raise AssertionError('startup mutation survived: '+name)


if __name__ == '__main__':
    main()
