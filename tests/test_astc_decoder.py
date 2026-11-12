"""Compare ASTC partition selection and endpoint boundaries with the pre-optimization decoder.

The reference is frozen GPL shader logic, not a second call to the candidate.
This covers these function domains, not full ASTC conformance or GPU performance.
"""
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]
ROOT = Path(__file__).resolve().parents[1]


def block(source, marker):
    start = source.index(marker)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def candidate(source):
    table = source[source.index("const uint encoding_values[22]"):source.index("// Input ASTC texture globals")]
    table = table.replace("const uint", "constexpr uint", 1).replace("= uint[](", "= {").replace(");", "};")
    helpers = ["uint Hash52(", "struct PartitionSetup {", "PartitionSetup SetupPartition(",
               "uint Select2DPartition(", "uint Encoding(EncodingData", "uint NumBits(EncodingData",
               "uint Div3Floor(", "uint Div3Ceil(", "uint Div5Floor(", "uint Div5Ceil(", "uint GetBitLength("]
    functions = [block(source, name) + (";" if name.startswith("struct") else "") for name in helpers]
    search = block(source, "void DecodeColorValues(")
    search = search[search.index("    int range_low = 1;"):search.index("    DecodeIntegerSequence")]
    search = search.replace("encoding_values.length()", "22")
    return "namespace candidate {\nuvec2 block_dims;\nstruct EncodingData {uint data;};\n" + \
        "constexpr uint JUST_BITS=0, QUINT=1, TRIT=2;\n" + table + "\n" + "\n".join(functions) + \
        "\nuint endpoint(uint num_values, uint color_data_bits){\n" + search + "return uint(range_low-1);}\n}\n"


PREFIX = r"""
#include <cstdint>
#include <cstdio>
#include <set>
#include <utility>
using uint=uint32_t;
struct uvec2 {uint x,y;};
struct uvec4 {
 uint x,y,z,w;
 uvec4(uint a,uint b,uint c,uint d):x(a),y(b),z(c),w(d){}
 uvec4():x(0),y(0),z(0),w(0){}
};
uvec4 operator*(uvec4 a,uint b){return {a.x*b,a.y*b,a.z*b,a.w*b};}
uvec4 operator+(uvec4 a,uvec4 b){return {a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w};}
uvec4 operator&(uvec4 a,uint b){return {a.x&b,a.y&b,a.z&b,a.w&b};}
uint bitfieldExtract(uint value,int start,int bits){return (value>>start)&((1u<<bits)-1);}
"""

MAIN = r"""
int main(){
 const std::pair<uint,uint> footprints[]={{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},
     {8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
 std::set<std::pair<uint,uint>> sizes;
 for(auto [x,y]:footprints){sizes.emplace(x,y);sizes.emplace(y,x);}
 uint64_t positions=0;
 for(auto [w,h]:sizes){
  reference::block_dims={w,h};candidate::block_dims={w,h};
  for(uint parts=2;parts<=4;parts++)for(uint seed=0;seed<1024;seed++){
   auto setup=candidate::SetupPartition(seed,parts);
   for(uint y=0;y<h;y++)for(uint x=0;x<w;x++){
    if(reference::Select2DPartition(seed,x,y,parts)!=candidate::Select2DPartition(setup,x,y)){
     std::fprintf(stderr,"partition mismatch %ux%u parts%u seed%u x%u y%u\n",w,h,parts,seed,x,y);
     return 2;
    }
    ++positions;
   }
  }
 }
 unsigned choices=0;
 for(uint count=0;count<=32;count++)for(uint bits=0;bits<=255;bits++){
  if(reference::endpoint(count,bits)!=candidate::endpoint(count,bits)){
   std::fprintf(stderr,"endpoint mismatch values%u bits%u\n",count,bits);return 3;
  }
  ++choices;
 }
 std::printf("ASTC: %llu partition positions, %u endpoint choices\n",
             (unsigned long long)positions,choices);
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True)
    args = parser.parse_args()
    shader = (ROOT / "gpu/vk/host_shaders/astc_decoder.comp").read_text(encoding="utf-8")
    reference = (ROOT / "tests/fixtures/astc_decoder_reference.h").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory() as folder:
        source = Path(folder) / "astc.cpp"
        exe = Path(folder) / "astc.exe"
        source.write_text(PREFIX + reference + candidate(shader) + MAIN, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(exe)], check=True)
        env = os.environ.copy()
        if os.name == "nt":
            compiler = Path(shutil.which(args.cxx) or args.cxx).resolve()
            env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
        subprocess.run([str(exe)], check=True, timeout=30, env=env)


if __name__ == "__main__":
    main()
