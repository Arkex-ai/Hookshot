"""Actual pack-file adapter and shared-handle reads; Linux links the production POSIX layer."""
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='romfs-file-') as tmp:
        folder = Path(tmp)
        source = folder/'source'
        source.mkdir()
        (source/'pattern').write_bytes(bytes((i * 17 + 3) % 251 for i in range(4096)))
        image = folder/'authored.pack'
        writer.pack(source, image)
        exe = folder/('file-test.exe' if os.name == 'nt' else 'file-test')
        cmd = [args.cxx, '-x', 'c', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
               str(ROOT/'tests/romfs_pack_file.c'), '-o', str(exe)]
        if os.name != 'nt':
            obj = folder/'posix.o'
            compat = [args.cxx, '-x', 'c', '-std=gnu11', '-O1', '-I'+str(ROOT/'host/posix'),
                      '-ffunction-sections', '-fdata-sections', '-c',
                      str(ROOT/'host/posix/win32_posix.c'), '-o', str(obj)]
            compiled = subprocess.run(compat, capture_output=True, text=True, timeout=120)
            if compiled.returncode:
                raise RuntimeError(compiled.stdout + compiled.stderr)
            cmd += ['-I'+str(ROOT/'host/posix'), '-x', 'none', str(obj),
                    '-Wl,--gc-sections', '-pthread', '-ldl', '-lm']
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120,
                                creationflags=0x08000000 if os.name == 'nt' else 0)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        result = subprocess.run([str(exe), str(image)], capture_output=True, text=True, timeout=60,
                                creationflags=0x08000000 if os.name == 'nt' else 0)
        if result.returncode:
            raise RuntimeError((result.returncode, result.stdout, result.stderr))
        print(result.stdout.strip())
        fixtures = importlib.util.spec_from_file_location('authored_pack_contract', ROOT/'tests/test_romfs_pack_reader.py')
        contract = importlib.util.module_from_spec(fixtures)
        fixtures.loader.exec_module(contract)
        for a, b in [('Case', 'case'), ('Café', 'CAFÉ')]:
            rows = sorted([(a.encode(), 0, b''), (b.encode(), 0, b'')], key=lambda row: row[0])
            image.write_bytes(contract.image(rows))
            result = subprocess.run([str(exe), str(image), 'check'], capture_output=True, timeout=15,
                                    creationflags=0x08000000 if os.name == 'nt' else 0)
            assert result.returncode == (2 if os.name == 'nt' else 0), (a, b, result.returncode)
        print('case-ambiguous images refused on Windows; distinct names retained on Linux')
        image.write_bytes(b'authored truncated file')
        result = subprocess.run([str(exe), str(image)], capture_output=True, timeout=15,
                                creationflags=0x08000000 if os.name == 'nt' else 0)
        assert result.returncode == 2, result.returncode
        print('truncated image refused by actual OS-backed open')


if __name__ == '__main__':
    main()
