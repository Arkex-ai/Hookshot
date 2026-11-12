"""Native C reader on authored packs, including malformed input and positional reads."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

RUN_ALL_ARGS = ['--cxx', '{cxx}']
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('romfs_pack', ROOT / 'nx2/romfs_pack.py')
writer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(writer)


def image(rows):
    """Independent format fixture: (UTF-8 name bytes, directory flag, contents)."""
    payload, index = bytearray(), bytearray()
    for name, directory, contents in rows:
        offset = 0 if directory else 64 + len(payload)
        index.extend(struct.pack('<IIQQ', len(name), directory, offset, len(contents)))
        index.extend(name)
        payload.extend(contents)
    at = 64 + len(payload)
    head = struct.pack('<8sIIQQQQIIQ', b'HROMFS1\0', len(rows), 0, at,
                       len(index), at, at + len(index), zlib.crc32(index), 0, 0)
    head = head[:52] + struct.pack('<I', zlib.crc32(head)) + head[56:]
    return bytearray(head + payload + index)


def fix_crc(data):
    at = struct.unpack_from('<Q', data, 16)[0]
    struct.pack_into('<I', data, 48, zlib.crc32(data[at:]))
    struct.pack_into('<I', data, 52, 0)
    struct.pack_into('<I', data, 52, zlib.crc32(data[:64]))
    return data


def exercise(exe, folder):
    checks = 0
    def run(data, arguments=(), code=0, expected=None, mode=None):
        nonlocal checks
        path = folder / 'fixture.pack'
        path.write_bytes(data)
        cmd = [str(exe), mode or ('read' if arguments else 'check'), str(path), *arguments]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=15,
                                creationflags=0x08000000 if os.name == 'nt' else 0)
        if result.returncode != code or (expected is not None and result.stdout.strip() != expected):
            raise AssertionError((arguments, result.returncode, code, result.stdout, expected, result.stderr))
        checks += 1
    good = image([(b'alpha', 0, b'0123456789'), (b'dir', 1, b''),
                  ('dir/é'.encode(), 0, b'unicode'), (b'empty', 0, b'')])
    run(good, expected='4')
    run(image([]), expected='0')
    for name, off, size, expected in [('alpha', 2, 3, b'234'), ('alpha', 8, 9, b'89'),
                                      ('alpha', 10, 4, b''), ('empty', 0, 8, b''),
                                      ('dir/é', 0, 7, b'unicode'), ('alpha', 0, 0, b'')]:
        run(good, [name.encode().hex(), str(off), str(size)], expected=expected.hex())
    for name, off in [('alpha', 11), ('dir', 0), ('absent', 0)]:
        run(good, [name.encode().hex(), str(off), '1'], code=3)
    run(good, [b'alpha'.hex(), '0', '5', 'fail'], code=3)
    run(good, [b'alpha'.hex(), '0', '5', 'partial'], code=3, expected='2:3031')
    run(good, code=2, mode='short-open')
    run(good, code=2, mode='short-index')
    rich = image(sorted([(b'alpha', 0, b'0'), (b'dir', 1, b''), (b'dir.ext', 0, b'sibling'),
                         (b'dir/child', 1, b''), (b'dir/child/deep', 0, b'deep'),
                         (b'dir/x', 0, b'x'), ('dir/é'.encode(), 0, b'utf8'),
                         (b'dir0', 0, b'next'), (b'emptydir', 1, b'')], key=lambda row: row[0]))
    listings = [('', 3, [('F', 'alpha', 1), ('D', 'dir', 0), ('F', 'dir.ext', 7),
                         ('F', 'dir0', 4), ('D', 'emptydir', 0)]),
                ('', 1, [('D', 'dir', 0), ('D', 'emptydir', 0)]),
                ('', 2, [('F', 'alpha', 1), ('F', 'dir.ext', 7), ('F', 'dir0', 4)]),
                ('', 0, []), ('dir', 3, [('D', 'child', 0), ('F', 'x', 1), ('F', 'é', 4)]),
                ('dir', 1, [('D', 'child', 0)]), ('dir', 2, [('F', 'x', 1), ('F', 'é', 4)]),
                ('dir/child', 3, [('F', 'deep', 4)]), ('emptydir', 3, [])]
    for directory, select, entries in listings:
        expected = '\n'.join(['count=' + str(len(entries))] +
                             [f'{kind}:{name.encode().hex()}:{size}' for kind, name, size in entries])
        for page in (1, 2, 99):
            run(rich, [directory.encode().hex(), str(select), str(page)], mode='list', expected=expected)
    run(image([]), ['', '3', '1'], mode='list', expected='count=0')
    for directory in ('absent', 'alpha', 'dir/x', 'dir/child/missing', 'dir/'):
        run(rich, [directory.encode().hex(), '3', '1'], mode='list', code=3)
    # The actual writer must be readable too; no copied image or game fixture.
    source = folder/'source'
    source.mkdir(exist_ok=True)
    (source/'payload').write_bytes(b'writer contract')
    output = folder/'writer.pack'
    writer.pack(source, output, replace=True)
    run(output.read_bytes(), [b'payload'.hex(), '0', '100'], expected=b'writer contract'.hex())
    for size in range(64):
        run(good[:size], code=2)
    run(good[:-1], code=2)
    for at in (0, 12, 48, 52, 56):
        corrupt = bytearray(good)
        corrupt[at] ^= 1
        run(corrupt, code=2)
    # Structurally valid mutation without a new index CRC.
    corrupt = bytearray(good)
    index_at = struct.unpack_from('<Q', corrupt, 16)[0]
    corrupt[index_at + 24] = ord('b')
    run(corrupt, code=2)
    for offset, value in [(8, 1000001), (16, 63), (24, (64 << 20) + 1),
                          (32, 0), (40, len(good) + 1), (56, 1), (12, 1)]:
        corrupt = bytearray(good)
        struct.pack_into('<I' if offset in (8, 12) else '<Q', corrupt, offset, value)
        struct.pack_into('<I', corrupt, 52, 0)
        struct.pack_into('<I', corrupt, 52, zlib.crc32(corrupt[:64]))
        run(corrupt, code=2)
    for name in (b'../escape', b'/absolute', b'a//b', b'a/', b'a\\b', b'NUL .txt',
                 b'COM0', b'a:b', b'bad\0name', b'\xc0\xaf', b'\xed\xa0\x80', b'\xf4\x90\x80\x80'):
        run(image([(name, 0, b'')]), code=2)
    for rows in [[(b'a', 0, b''), (b'a', 0, b'')],
                 [(b'b', 0, b''), (b'a', 0, b'')],
                 [(b'dir/child', 0, b'')],
                 [(b'dir', 0, b''), (b'dir/child', 0, b'')],
                 [(b'dir', 1, b'nonempty')]]:
        run(image(rows), code=2)
    for record_offset, value in [(0, 0xffffffff), (4, 2), (8, 65), (16, 0xffffffffffffffff)]:
        corrupt = bytearray(good)
        struct.pack_into('<I' if record_offset < 8 else '<Q', corrupt, index_at + record_offset, value)
        run(fix_crc(corrupt), code=2)
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='romfs-native-') as tmp:
        folder = Path(tmp)
        source = folder/'driver.c'
        driver = (ROOT/'tests/romfs_pack_reader.c').read_text(encoding='utf-8')
        header = (ROOT/'host/romfs_pack.h').read_text(encoding='utf-8')
        source.write_text(driver.replace('../host/romfs_pack.h', 'reader.h'), encoding='utf-8')
        def build(text):
            (folder/'reader.h').write_text(text, encoding='utf-8')
            exe = folder/('reader.exe' if os.name == 'nt' else 'reader')
            compiled = subprocess.run([args.cxx, '-x', 'c', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                                       str(source), '-o', str(exe)], capture_output=True, text=True,
                                      creationflags=0x08000000 if os.name == 'nt' else 0)
            if compiled.returncode:
                raise AssertionError('reader fixture compilation failed:\n' + compiled.stdout + compiled.stderr)
            return exe
        checks = exercise(build(header), folder)
        rejected = []
        if args.mutations:
            edits = {
                'index-crc': ('if (romfs_pack_crc(p.index, (size_t)index_size) != romfs_pack_u32(h + 48)) goto bad;', ''),
                'safe-path': ('!romfs_pack_name(p.index + cursor, e->length)', '0'),
                'read-eof-clamp': ('if (e->size - offset < size) size = (size_t)(e->size - offset);', ''),
                'directory-descendants': ('if (slash) {', 'if (0) {'),
                'count-after-reading': ('copy.cursor = romfs_pack_lower(p, copy.prefix, copy.prefix_length);', '')}
            edits['partial-byte-count'] = ('*done = got;', '*done = 0;')
            for name, (old, new) in edits.items():
                assert header.count(old) == 1
                try:
                    exercise(build(header.replace(old, new)), folder)
                except AssertionError:
                    rejected.append(name)
                else:
                    raise AssertionError('semantic mutation survived: ' + name)
        print(json.dumps(dict(nativeChecks=checks, rejectedMutations=rejected)))


if __name__ == '__main__':
    main()
