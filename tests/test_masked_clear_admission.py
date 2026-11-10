"""Execute masked-clear resource ownership, publication and retry with authored failures."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]

ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, default=ROOT)
    p.add_argument('--headers', type=Path, required=True)
    p.add_argument('--cxx', default='g++')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    source = (a.source / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    actual = 'template<class CI>\n' + block(source, 'VkResult create_cached(') + '\n' + '\n'.join(block(source, m) for m in ['VkResult cached_graphics(', 'std::vector<uint32_t> spirv_location(', 'VkPipeline clear_pipeline('])
    actual = actual.replace('std::vector<uint32_t>', 'FaultVector<uint32_t, FragmentWords>')
    old = 'std::unordered_set<uint64_t> failed'
    assert actual.count(old) == 1
    actual = actual.replace(old, 'FaultSet<uint64_t, Failure> failed')
    caller = block(source, 'void clear_color_target(')
    consumer = caller[caller.index('    const VkPipeline pl = clear_pipeline('):caller.rfind('}')]
    fixture = (ROOT / 'tests/masked_clear_admission_test.cpp').read_text(encoding='utf-8').replace('/* ACTUAL */', actual).replace('/* CONSUMER */', consumer)
    with tempfile.TemporaryDirectory(prefix='masked-clear-admission-') as temp:
        cpp, exe = Path(temp) / 'fixture.cpp', Path(temp) / 'fixture.exe'
        cpp.write_text(fixture, encoding='utf-8')
        subprocess.run([a.cxx, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers',
                        '-pthread', '-I' + str(a.headers),
                        *(['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'] if a.sanitize else []),
                        str(cpp), '-o', str(exe)], check=True)
        cases = ['play-cache-hit', 'play-cache-miss', 'healthy', 'layout', 'vertex', 'fragment', 'pipeline', 'layout-null', 'vertex-null', 'fragment-null',
                 'host-oom', 'unknown', 'owner-commit', 'fragment-alloc', 'failure-metadata', 'independent',
                 'valid-after-failure', 'concurrent', 'consumer', 'consumer-cached', 'float', 'unsigned', 'signed',
                 'index7', 'msaa', 'depth', 'stencil', 'distinct-mask', 'distinct-format', 'distinct-samples',
                 'distinct-depth', 'distinct-index', 'signed-after-unsigned']
        for case in cases:
            subprocess.run([str(exe), case], check=True, timeout=20)
    print(f'{len(cases)} masked-clear admission/ownership/policy cases passed')


if __name__ == '__main__':
    main()
