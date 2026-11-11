"""Run the temporal dispatch and DLSS feature-reuse decisions without Vulkan or NGX."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]

FIXTURE = r'''
#include <cstdint>
#include <cstdio>
#include "gpu/vk/upscaler_rows.h"
namespace ur = mkwvk::upscaler_rows;

struct TestEngine {
    const char* name = "fixture";
    bool feature = false, feature_aa = false;
    uint32_t feature_ow = 0, feature_oh = 0, feature_rw = 0, feature_rh = 0;
    uint32_t min_rw = 0, min_rh = 0;
    unsigned feature_preset = 0, creations = 0;

    int prepare(int, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool native_aa,
                uint32_t* in_w, uint32_t* in_h, const char**) {
        auto& s = *this;
        const unsigned preset = 0;
        (void)native_aa;
        /* REUSE */

        // Model a scaled feature that can accept full-size input, just as NGX can.
        feature = true;
        feature_aa = rw == ow && rh == oh;
        feature_ow = feature_rw = ow;
        feature_oh = feature_rh = oh;
        min_rw = feature_aa ? ow : ow / 2;
        min_rh = feature_aa ? oh : oh / 2;
        ++creations;
        return 2;
    }

    int prepare(int cmd, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh,
                uint32_t* iw, uint32_t* ih, const char** why) {
        return prepare(cmd, rw, rh, ow, oh, false, iw, ih, why);
    }

    void release() { feature = false; }
};

struct Engine { TestEngine* e; };
struct State { TestEngine* drawing = nullptr; uint64_t last_frame = 0; int drawn_mode = ur::GAME; };
static int texture_frame_no() { return 1; }
static void descriptors_disturbed() {}
template <class... Args> static void logf(const char*, Args...) {}

static void step(State& s, Engine engine, int mode, uint32_t rw, uint32_t rh) {
    const uint32_t ow = 3440, oh = 1440;
    const int cmd = 0;
    /* DISPATCH */
    (void)ready;
    /* DRAWN_MODE */
}

int main() {
    State state;
    TestEngine ngx, fsr;
    step(state, {&ngx}, ur::DLSS, 1720, 720);
    step(state, {&ngx}, ur::DLAA, 1720, 720);
    step(state, {&ngx}, ur::DLAA, 2294, 960);
    if (ngx.creations != 1 || ngx.feature_ow != 3440 || ngx.feature_oh != 1440) {
        std::fprintf(stderr, "The queued scaled frames changed the DLSS feature\n");
        return 1;
    }
    step(state, {&ngx}, ur::DLAA, 3440, 1440);
    step(state, {&ngx}, ur::DLAA, 3440, 1440);
    if (ngx.creations != 2 || !ngx.feature_aa) return 2;
    step(state, {&fsr}, ur::FSR4_AA, 1720, 720);
    if (ngx.feature || fsr.feature_ow != 1720) return 3;

    State auto_native;
    TestEngine already_native;
    step(auto_native, {&already_native}, ur::DLSS, 3440, 1440);
    step(auto_native, {&already_native}, ur::DLAA, 3440, 1440);
    if (already_native.creations != 1 || !already_native.feature_aa) return 4;

    State startup;
    TestEngine startup_ngx;
    step(startup, {&startup_ngx}, ur::DLAA, 1720, 720);
    if (startup_ngx.creations != 1 || startup_ngx.feature_ow != 1720) return 5;
    std::puts("PASS: queued scaled frames, native promotion, reuse and engine switch");
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    chain = (args.source / "gpu/vk/vk_temporal.cpp").read_text(encoding="utf-8")
    dlss = (args.source / "gpu/vk/vk_dlss.cpp").read_text(encoding="utf-8")
    chain = chain[chain.index('if (!dview) return fail("no view of the scene depth");'):]
    dispatch = chain[chain.index("\n"):chain.index("    if (ready < 0)")]
    reuse_start = dlss.index("    if (s.feature && s.feature_ow == ow")
    reuse = dlss[reuse_start:dlss.index("    // (why it is made:", reuse_start)]
    assignment = "s.drawn_mode = draw_mode;"
    text = FIXTURE.replace("/* DISPATCH */", dispatch).replace("/* REUSE */", reuse)
    text = text.replace("/* DRAWN_MODE */", assignment if assignment in chain else "")
    with tempfile.TemporaryDirectory(prefix="dlss-native-transition-") as folder:
        source, exe = Path(folder) / "fixture.cpp", Path(folder) / "fixture.exe"
        source.write_text(text, encoding="utf-8")
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I" + str(args.source),
                        str(source), "-o", str(exe)], check=True, timeout=30)
        raise SystemExit(subprocess.run([str(exe)], timeout=30).returncode)


if __name__ == "__main__":
    main()
