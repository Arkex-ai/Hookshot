"""Optional read-only RomFS pack, created from a folder without changing it.

Internal format candidate; runtime integration is not complete yet.
"""
import argparse
import os
from pathlib import Path
import stat
import struct
import tempfile
import zlib

MAGIC = b'HROMFS1\0'
HEADER = struct.Struct('<8sIIQQQQIIQ')
ENTRY = struct.Struct('<IIQQ')
MAX_ENTRIES = 1_000_000
MAX_INDEX = 64 << 20
CHUNK = 1 << 20


def linked(info):
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) & 1024)


def identity(info):
    # Windows ctime is deprecated and can differ between path/handle APIs.
    # Use creation time there; POSIX ctime remains a change detector.
    change = getattr(info, 'st_birthtime_ns', 0) if os.name == 'nt' else info.st_ctime_ns
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, change)


def name_bytes(name):
    data = name.encode('utf-8', errors='strict')
    if not data or len(data) > 0x300:
        raise ValueError('pack path is empty or too long')
    for part in name.split('/'):
        if (not part or part in ('.', '..') or part.endswith((' ', '.'))
                or any(ord(c) < 32 or c in '\\:*?"<>|' for c in part)):
            raise ValueError('pack path is not portable: ' + name)
        stem = part.split('.')[0].rstrip(' ').upper()
        if stem in ('CON', 'PRN', 'AUX', 'NUL', 'CONIN$', 'CONOUT$', 'CLOCK$') or (
                len(stem) == 4 and stem[:3] in ('COM', 'LPT') and stem[3] in '0123456789¹²³'):
            raise ValueError('pack path uses a Windows device name: ' + name)
    return data


def inventory(root):
    result = []
    def visit(folder):
        for child in folder.iterdir():
            info = child.lstat()
            if linked(info) or not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
                raise ValueError('pack input must contain ordinary files/directories only: ' + str(child))
            rel = child.relative_to(root).as_posix()
            result.append((name_bytes(rel), child, stat.S_ISDIR(info.st_mode), identity(info)))
            if len(result) > MAX_ENTRIES:
                raise ValueError('pack contains too many entries')
            if stat.S_ISDIR(info.st_mode):
                visit(child)
    visit(root)
    result.sort(key=lambda item: item[0])
    # Folder case behavior must not depend on the machine that packed it.
    folded = set()
    for name, _, _, _ in result:
        key = name.decode('utf-8').casefold()
        if key in folded:
            raise ValueError('pack has case-ambiguous paths')
        folded.add(key)
    if sum(ENTRY.size + len(row[0]) for row in result) > MAX_INDEX:
        raise ValueError('pack index is too large')
    return result


def pack(source, destination, *, replace=False, progress=None):
    source, destination = Path(source).absolute(), Path(destination).absolute()
    info = source.lstat()
    if linked(info) or not stat.S_ISDIR(info.st_mode):
        raise ValueError('pack source must be an ordinary directory')
    root = source.resolve(strict=True)
    parent = destination.parent.resolve(strict=True)
    target = parent / destination.name
    if target == root or root in target.parents:
        raise ValueError('pack output must be outside the source directory')
    if target.exists() or target.is_symlink():
        if linked(target.lstat()) or not target.is_file() or not replace:
            raise ValueError('pack output already exists or is not an ordinary file')
    rows = inventory(root)
    initial = [(row[0], row[2], row[3]) for row in rows]
    fd, temporary = tempfile.mkstemp(prefix='.' + target.name + '-', suffix='.part', dir=parent)
    try:
        with os.fdopen(fd, 'w+b') as output:
            if output.write(bytes(HEADER.size)) != HEADER.size:
                raise OSError('short header reservation while packing')
            index = bytearray()
            for name, path, directory, expected in rows:
                offset, size = (0, 0) if directory else (output.tell(), expected[3])
                if not directory:
                    # Check the opened object, not just the pathname checked during enumeration.
                    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0)
                    with os.fdopen(os.open(path, flags), 'rb') as item:
                        opened = identity(os.fstat(item.fileno()))
                        if opened != expected or linked(path.lstat()):
                            raise ValueError('pack source changed before reading: %r != %r' % (opened, expected))
                        remaining = size
                        while remaining:
                            data = item.read(min(CHUNK, remaining))
                            if not data:
                                raise ValueError('pack source ended early')
                            if output.write(data) != len(data):
                                raise OSError('short write while packing')
                            remaining -= len(data)
                            if progress:
                                progress(name.decode('utf-8'), size - remaining, size)
                        if item.read(1) or identity(os.fstat(item.fileno())) != expected:
                            raise ValueError('pack source changed while reading')
                index.extend(ENTRY.pack(len(name), int(directory), offset, size))
                index.extend(name)
            # Added/removed/changed entries invalidate the transaction too.
            final = [(row[0], row[2], row[3]) for row in inventory(root)]
            if final != initial:
                raise ValueError('pack source tree changed while packing')
            at = output.tell()
            if output.write(index) != len(index):
                raise OSError('short index write while packing')
            total = output.tell()
            fields = (MAGIC, len(rows), 0, at, len(index), at, total,
                      zlib.crc32(index), 0, 0)
            raw = HEADER.pack(*fields)
            header = HEADER.pack(*fields[:8], zlib.crc32(raw), fields[9])
            output.seek(0)
            if output.write(header) != len(header):
                raise OSError('short header write while packing')
            output.flush()
            os.fsync(output.fileno())
        if replace:
            os.replace(temporary, target)
        elif os.name == 'nt':
            # Windows rename refuses an existing target, including on exFAT.
            os.rename(temporary, target)
        else:
            # Atomic no-clobber publication; a competing output is never overwritten.
            os.link(temporary, target)
            os.unlink(temporary)
        return dict(entries=len(rows), payload=at - HEADER.size, bytes=total)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source')
    parser.add_argument('destination')
    parser.add_argument('--replace', action='store_true', help='atomically replace an existing pack')
    args = parser.parse_args()
    print(pack(args.source, args.destination, replace=args.replace))


if __name__ == '__main__':
    main()
