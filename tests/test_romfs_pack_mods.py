"""Pack routing with the real mod index and authored files, using a controlled prompt provider.

Actual scan/order, disabled/nested mods, additions, directory deduplication, image authority,
host base assets and redirect/overlay priority. The controlled prompt provider does not test asset generation.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ['--cxx', '{cxx}']
ROOT = Path(__file__).resolve().parents[1]
HARNESS = Path(__file__).with_suffix('.c')

def run(cmd):
    result=subprocess.run(cmd,capture_output=True,text=True,timeout=120,
        creationflags=0x08000000 if os.name=='nt' else 0)
    if result.returncode:
        raise RuntimeError(result.stdout+result.stderr)
    return result.stdout

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx',default='clang++')
    args=parser.parse_args()
    tree=ROOT
    spec=importlib.util.spec_from_file_location('authored_fs',ROOT/'tests/test_romfs_pack_fs.py')
    fixture=importlib.util.module_from_spec(spec);spec.loader.exec_module(fixture)
    spec=importlib.util.spec_from_file_location('startup_blocks',ROOT/'tests/test_romfs_pack_startup.py')
    blocks=importlib.util.module_from_spec(spec);spec.loader.exec_module(blocks)
    driver=(ROOT/'tests/romfs_pack_fs.c').read_text(encoding='utf-8').split('static int test_main(')[0]
    for marker in ('int hos_mods_romfs(', 'int hos_mods_dir_extras('):
        driver=driver.replace(blocks.block(driver,marker),'')
    driver+=HARNESS.read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='romfs-mods-') as raw:
        folder=Path(raw);source=folder/'romfs';source.mkdir();mods=folder/'mods';mods.mkdir()
        def put(path,data):path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)
        for name,data in [('pattern',b'BASE'),('base-only',b'BASEONLY'),('UI/generated',b'ORIGINAL'),
                           ('UI/prompt',b'ORIGINAL'),('UI/fixture.rsizetable.zs',b'BASETABLE')]:put(source/name,data)
        image=folder/'assets.pack';fixture.writer.pack(source,image)
        put(source/'base-only',b'STALEONLY');put(source/'stale',b'STALE')
        for name,data in [('01-first/romfs/pattern',b'FIRST'),('02-last/romfs/pattern',b'LAST'),
                          ('02-last/romfs/new-dir/new',b'NEW'),('02-last/romfs/new-only',b'ADDED'),
                          ('02-last/romfs/UI/prompt',b'MODPROMPT'),('02-last/romfs/UI/fixture.rsizetable.zs',b'MODTABLE'),
                          ('-disabled/romfs/pattern',b'BAD'),('_disabled/romfs/pattern',b'BAD'),
                          ('contents/title/03-nested/romfs/nested-only',b'NESTED')]:put(mods/name,data)
        generated,merged,wide=folder/'generated',folder/'merged',folder/'wide'
        generated.write_bytes(b'PROMPT');merged.write_bytes(b'MERGED');wide.write_bytes(b'WIDE')
        overlay=folder/'overlay';put(overlay/'pattern',b'OVERLAY')
        (folder/'authored_hos.h').write_text(fixture.HOS,encoding='utf-8')
        for file in ('nn_fs.c','mods.c'):
            text=(tree/'host'/file).read_text(encoding='utf-8')
            (folder/file).write_text(text.replace('#include "hos.h"','#include "authored_hos.h"'),encoding='utf-8')
        (folder/'driver.c').write_text(driver,encoding='utf-8')
        exe=folder/('probe.exe' if os.name=='nt' else 'probe')
        cxx=args.cxx
        mods_object=folder/'mods.o'
        mod_cmd=[cxx,'-x','c','-std=gnu11','-O1','-I'+str(tree/'host'),'-include',str(tree/'gpu/vk/compat/hookshot_env.h'),
                 '-c',str(folder/'mods.c'),'-o',str(mods_object)]
        cmd=[cxx,'-x','c','-std=gnu11','-O1','-I'+str(tree/'host'),'-include',str(tree/'gpu/vk/compat/hookshot_env.h'),
             str(folder/'driver.c'),'-o',str(exe)]
        if os.name!='nt':
            obj=folder/'posix.o'
            run([cxx,'-x','c','-O1','-I'+str(tree/'host/posix'),'-ffunction-sections','-fdata-sections','-c',str(tree/'host/posix/win32_posix.c'),'-o',str(obj)])
            mod_cmd+=['-D_GNU_SOURCE','-I'+str(tree/'host/posix')]
            cmd+=['-D_GNU_SOURCE','-I'+str(tree/'host/posix'),'-x','none',str(obj),'-Wl,--gc-sections','-pthread','-ldl','-lm']
        run(mod_cmd);cmd+=['-x','none',str(mods_object)]
        run(cmd);output=run([str(exe),str(image),str(source),str(mods),str(generated),str(merged),str(wide),str(overlay)])
    print(output.strip())

if __name__ == "__main__":
    main()
