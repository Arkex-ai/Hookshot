"""Authored files only: pack contents and transaction failures, no game data."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('romfs_pack', ROOT / 'nx2/romfs_pack.py')
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)


def decode(path):
    """Independent consumer of the proposed disk contract, not the writer's structs."""
    data = path.read_bytes()
    fields = struct.unpack('<8sIIQQQQIIQ', data[:64])
    magic, count, flags, at, length, end, total, crc, hcrc, reserved = fields
    assert magic == b'HROMFS1\0' and not flags and not reserved
    assert end == at and total == len(data) and at + length == total
    raw = bytearray(data[:64])
    raw[52:56] = bytes(4)
    assert zlib.crc32(raw) == hcrc and zlib.crc32(data[at:]) == crc
    result = {}
    for _ in range(count):
        n, directory, offset, size = struct.unpack_from('<IIQQ', data, at)
        at += 24
        name = data[at:at + n].decode('utf-8')
        at += n
        assert name not in result and directory in (0, 1)
        if directory:
            assert offset == size == 0
            result[name] = None
        else:
            assert offset >= 64 and offset + size <= end
            result[name] = data[offset:offset + size]
    assert at == len(data)
    return result


class PackTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='authored-romfs-')
        self.addCleanup(self.temporary.cleanup)
        # pack() canonicalizes its input; Windows TEMP may use an 8.3 alias.
        # Match that path before installing lstat fault injection by identity.
        self.root = Path(self.temporary.name).resolve()
        self.source = self.root / 'source'
        self.source.mkdir()
        self.target = self.root / 'assets.pack'

    def assert_no_partial(self):
        self.assertEqual(list(self.root.glob('*.part')), [])

    def test_contents_empty_directories_unicode_and_determinism(self):
        (self.source / 'empty-dir').mkdir()
        (self.source / 'nested').mkdir()
        (self.source / 'empty').write_bytes(b'')
        (self.source / 'nested/cafe-é').write_bytes(b'authored\x00bytes' * 37)
        before = {p.relative_to(self.source): p.read_bytes() for p in self.source.rglob('*') if p.is_file()}
        packer.pack(self.source, self.target)
        self.assertEqual(decode(self.target), {'empty': b'', 'empty-dir': None,
                         'nested': None, 'nested/cafe-é': b'authored\x00bytes' * 37})
        other = self.root / 'second.pack'
        packer.pack(self.source, other)
        self.assertEqual(other.read_bytes(), self.target.read_bytes())
        self.assertEqual(before, {p.relative_to(self.source): p.read_bytes()
                                 for p in self.source.rglob('*') if p.is_file()})
        self.assert_no_partial()

    def test_existing_output_and_inside_source_refused(self):
        self.target.write_bytes(b'keep previous output')
        with self.assertRaises(ValueError):
            packer.pack(self.source, self.target)
        with self.assertRaises(ValueError):
            packer.pack(self.source, self.source / 'recursive.pack')
        self.assertEqual(self.target.read_bytes(), b'keep previous output')
        self.assert_no_partial()

    def test_unchanged_file_with_preserved_old_mtime(self):
        path = self.source / 'older-content'
        path.write_bytes(b'authored older content')
        os.utime(path, ns=(946684800000000000, 946684800000000000))
        packer.pack(self.source, self.target)
        self.assertEqual(decode(self.target)['older-content'], b'authored older content')

    @unittest.skipUnless(os.name == 'nt', 'Windows directory metadata size has no payload meaning')
    def test_windows_directory_metadata_size_can_change_without_source_change(self):
        directory = self.source / 'nested'
        directory.mkdir()
        (directory / 'payload').write_bytes(b'authored bytes')
        original = Path.lstat
        visits = 0

        class DirectoryStat:
            def __init__(self, info, size):
                self.info, self.st_size = info, size

            def __getattr__(self, name):
                return getattr(self.info, name)

        def observed(path, *args, **kwargs):
            nonlocal visits
            info = original(path, *args, **kwargs)
            if path == directory:
                visits += 1
                return DirectoryStat(info, 4096 if visits == 1 else 0)
            return info

        with mock.patch.object(Path, 'lstat', observed):
            packer.pack(self.source, self.target)
        self.assertGreaterEqual(visits, 2)
        self.assertEqual(decode(self.target), {'nested': None, 'nested/payload': b'authored bytes'})

    def test_directory_identity_changes_still_preserve_previous_output(self):
        directory = self.source / 'nested'
        directory.mkdir()
        (directory / 'payload').write_bytes(b'authored bytes')
        original = Path.lstat

        class ReplacedDirectory:
            def __init__(self, info):
                self.info, self.st_ino = info, info.st_ino + 1

            def __getattr__(self, name):
                return getattr(self.info, name)

        visits = 0

        def observed(path, *args, **kwargs):
            nonlocal visits
            info = original(path, *args, **kwargs)
            if path == directory:
                visits += 1
                if visits > 1:
                    return ReplacedDirectory(info)
            return info

        self.target.write_bytes(b'previous output')
        with mock.patch.object(Path, 'lstat', observed), self.assertRaises(ValueError):
            packer.pack(self.source, self.target, replace=True)
        self.assertGreaterEqual(visits, 2)
        self.assertEqual(self.target.read_bytes(), b'previous output')
        self.assert_no_partial()

    def test_source_change_keeps_previous_output(self):
        path = self.source / 'payload'
        path.write_bytes(b'a' * (packer.CHUNK + 7))
        self.target.write_bytes(b'previous')
        def change(*_):
            with path.open('ab') as file:
                file.write(b'changed')
        with self.assertRaises(ValueError):
            packer.pack(self.source, self.target, replace=True, progress=change)
        self.assertEqual(self.target.read_bytes(), b'previous')
        self.assert_no_partial()

    def test_cancellation_keeps_previous_output(self):
        (self.source / 'payload').write_bytes(b'authored')
        self.target.write_bytes(b'previous')
        def cancel(*_):
            raise KeyboardInterrupt()
        with self.assertRaises(KeyboardInterrupt):
            packer.pack(self.source, self.target, replace=True, progress=cancel)
        self.assertEqual(self.target.read_bytes(), b'previous')
        self.assert_no_partial()

    def test_failed_publication_keeps_previous_output(self):
        (self.source / 'payload').write_bytes(b'authored')
        self.target.write_bytes(b'previous')
        with mock.patch.object(packer.os, 'replace', side_effect=OSError('authored publication failure')):
            with self.assertRaises(OSError):
                packer.pack(self.source, self.target, replace=True)
        self.assertEqual(self.target.read_bytes(), b'previous')
        self.assert_no_partial()

    def test_short_write_and_full_disk_never_publish(self):
        original = packer.os.fdopen
        class FaultyOutput:
            def __init__(self, file, full):
                self.file, self.full = file, full
            def __enter__(self):
                return self
            def __exit__(self, *args):
                return self.file.__exit__(*args)
            def __getattr__(self, name):
                return getattr(self.file, name)
            def write(self, data):
                if self.full:
                    raise OSError(28, 'authored disk full')
                return self.file.write(data[:-1])
        self.target.write_bytes(b'previous')
        for full in (False, True):
            def opened(fd, mode):
                file = original(fd, mode)
                return FaultyOutput(file, full) if mode == 'w+b' else file
            with self.subTest(full=full), mock.patch.object(packer.os, 'fdopen', side_effect=opened):
                with self.assertRaises(OSError):
                    packer.pack(self.source, self.target, replace=True)
            self.assertEqual(self.target.read_bytes(), b'previous')
            self.assert_no_partial()

    def test_competing_output_is_not_clobbered(self):
        (self.source / 'payload').write_bytes(b'authored')
        def competing_output(*_):
            self.target.write_bytes(b'other producer')
        with self.assertRaises(OSError):
            packer.pack(self.source, self.target, progress=competing_output)
        self.assertEqual(self.target.read_bytes(), b'other producer')
        self.assert_no_partial()

    def test_added_file_invalidates_transaction(self):
        (self.source / 'payload').write_bytes(b'authored')
        self.target.write_bytes(b'previous')
        def added(*_):
            (self.source / 'added').write_bytes(b'new')
        with self.assertRaises(ValueError):
            packer.pack(self.source, self.target, replace=True, progress=added)
        self.assertEqual(self.target.read_bytes(), b'previous')
        self.assert_no_partial()

    def test_nonportable_and_case_ambiguous_names_refused(self):
        for name in ('../escape', 'a//b', 'a\\b', 'NUL.txt', 'NUL .txt', 'COM0',
                     'COM1.bin', 'a.', 'a ', 'a:b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                packer.name_bytes(name)
        (self.source / 'Mixed').write_bytes(b'a')
        (self.source / 'mixed').write_bytes(b'b')
        # On a case-insensitive filesystem those names denote the same file.
        if len(list(self.source.iterdir())) == 2:
            with self.assertRaises(ValueError):
                packer.pack(self.source, self.target)

    def test_links_refused(self):
        outside = self.root / 'outside'
        outside.write_bytes(b'private fixture')
        try:
            (self.source / 'link').symlink_to(outside)
        except OSError:
            self.skipTest('creating symlinks requires platform privilege')
        with self.assertRaises(ValueError):
            packer.pack(self.source, self.target)
        self.assertFalse(self.target.exists())


if __name__ == '__main__':
    unittest.main()
