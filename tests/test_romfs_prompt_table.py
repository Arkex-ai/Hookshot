"""Actual prompt table discovery with a packed backend, stale loose files and no loose root."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ['--cxx', '{cxx}']
ROOT = Path(__file__).resolve().parents[1]


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT/'tests'/('test_'+name+'.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--prompts-source', type=Path, default=ROOT/'host/prompts.c')
    args = parser.parse_args()
    fs, blocks = load('romfs_pack_fs'), load('romfs_pack_startup')
    with tempfile.TemporaryDirectory(prefix='packed-prompt-table-') as raw:
        tmp = Path(raw)
        base = tmp/'base'
        resource = base/'System/Resource'
        resource.mkdir(parents=True)
        for name in ['ResourceSizeTable.B.rsizetable.zs', 'ResourceSizeTable.A.rsizetable.zs']:
            (resource/name).write_bytes(b'authored')
        (resource/'ResourceSizeTable.0.rsizetable.zs').mkdir()  # directories do not count
        (resource/'ResourceSizeTable.0.rsizetable.zs/nested').write_bytes(b'ignore')
        (resource/'unrelated').write_bytes(b'ignore')
        image = tmp/'image.pack'
        fs.writer.pack(base, image)
        stale = tmp/'stale/System/Resource'
        stale.mkdir(parents=True)
        (stale/'ResourceSizeTable.0.rsizetable.zs').write_bytes(b'stale')
        (tmp/'authored_hos.h').write_text(fs.HOS, encoding='utf-8')
        actual = (ROOT/'host/nn_fs.c').read_text(encoding='utf-8')
        (tmp/'nn_fs.c').write_text(actual.replace('#include "hos.h"', '#include "authored_hos.h"'), encoding='utf-8')
        driver = (ROOT/'tests/romfs_pack_fs.c').read_text(encoding='utf-8').split('static unsigned checks;')[0]
        driver += '\n' + blocks.block(args.prompts_source.read_text(encoding='utf-8'), 'static int game_table(')
        driver += r'''
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s line%d\n",#x,__LINE__); return 1; } } while(0)
int main(int argc,char **argv) {
    CHECK(argc==5);
    char out[1024];
    const char *expected="System/Resource/ResourceSizeTable.A.rsizetable.zs";
    hos_romfs_dir=argv[2];
    CHECK(game_table(out,sizeof out) && !strcmp(out,expected));
    CHECK(hos_fs_pack_open(argv[1]));
    hos_romfs_dir=argv[3];
    CHECK(game_table(out,sizeof out) && !strcmp(out,expected));
    hos_romfs_dir=argv[4];
    CHECK(game_table(out,sizeof out) && !strcmp(out,expected));
    CHECK(!game_table(out,4));
    CHECK(!hos_fs_packed_find("System/Resource/Absent.",".rsizetable.zs",out,sizeof out));
    CHECK(!hos_fs_packed_find("System/Resource/ResourceSizeTable.",".rsizetable.zs",NULL,sizeof out));
    CHECK(!hos_fs_packed_find("System/Resource/ResourceSizeTable.",".rsizetable.zs",out,0));
    puts("PASS folder, packed with stale loose root, image-only, bounded output and first-file rule");
    return 0;
}
'''
        source, exe = tmp/'driver.c', tmp/('probe.exe' if os.name=='nt' else 'probe')
        source.write_text(driver, encoding='utf-8')
        cmd = [args.cxx, '-x', 'c', '-std=gnu11', '-O1', '-Wall', '-Werror=implicit-function-declaration',
               '-I'+str(ROOT/'host'), '-include', str(ROOT/'gpu/vk/compat/hookshot_env.h'), str(source), '-o', str(exe)]
        if os.name!='nt':
            obj=tmp/'posix.o'
            fs.checked([args.cxx,'-x','c','-std=gnu11','-O1','-I'+str(ROOT/'host/posix'),'-ffunction-sections','-fdata-sections',
                        '-c',str(ROOT/'host/posix/win32_posix.c'),'-o',str(obj)])
            cmd += ['-D_GNU_SOURCE','-I'+str(ROOT/'host/posix'),'-x','none',str(obj),'-Wl,--gc-sections','-pthread','-ldl','-lm']
        fs.checked(cmd)
        print(fs.checked([str(exe),str(image),str(base),str(tmp/'stale'),str(tmp/'absent')]).stdout.strip())


if __name__=='__main__':
    main()
