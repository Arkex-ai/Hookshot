// Controlled completion, ownership, idle-release and mutex checks for the real GPU callbacks.
// No Vulkan device or game data is needed. Production blocks are inserted by the Python driver.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
using VkSemaphore = uintptr_t;
VkSemaphore timeline = 1;
uint64_t signaled = 20, gpu_done = 20, cpu_frame = 1;
unsigned timeline_queries = 0;
uint64_t completed() {
    ++timeline_queries;
    return gpu_done;
}
uint64_t texture_frame_no() { return cpu_frame; }
void need(bool ok, const char *why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", why);
        std::exit(2);
    }
}
// TIMELINE_PRODUCTION
unsigned released_engine = 0, freed_engine = 0, freed_owned = 0, drop_calls = 0;
bool selected = false, owned = false;
struct Engine {
    const char *name = "fixture";
    void release();
} engine;
struct State {
    std::mutex mu;
    // DEAD_PRODUCTION
    std::vector<Dead> dead;
    uint64_t asked = 0, last_frame = 1;
    std::atomic<bool> holding{false};
    Engine *drawing = nullptr;
};
State &st() {
    static State s;
    return s;
}
bool temporal_wanted() { return selected; }
void logf(const char *, ...) {}
void drop_all();
// RETIRE_PRODUCTION
void Engine::release() {
    ++released_engine;
    later([] { ++freed_engine; });
}
void drop_all() {
    ++drop_calls;
    if (owned) {
        owned = false;
        later([] { ++freed_owned; });
    }
}
// IDLE_PRODUCTION
void make_retirement(std::function<void()> f) {
    std::lock_guard lk(st().mu);
    later(std::move(f));
}
void collect_direct() {
    std::lock_guard lk(st().mu);
    FREE_CALL;
}
void collect_idle() { IDLE_CALL; }
int main(int argc, char **argv) {
    need(argc == 2, "one case");
    const char *mode = argv[1];
    unsigned a = 0, b = 0;
    if (!std::strcmp(mode, "empty")) {
        collect_direct();
        collect_idle();
        need(st().dead.empty() && !timeline_queries, "empty does not query GPU");
    } else if (!std::strcmp(mode, "pending-age")) {
        make_retirement([&] { ++a; });
        for (cpu_frame = 2; cpu_frame < 102; cpu_frame++)
            collect_direct();
        need(a == 0 && st().dead.size() == 1, "CPU frames cannot release pending GPU use");
        gpu_done = 21;
        collect_direct();
        need(a == 1 && st().dead.empty(), "completion collects once");
    } else if (!std::strcmp(mode, "completed-young")) {
        make_retirement([&] { ++a; });
        gpu_done = 21;
        collect_direct();
        need(a == 1 && st().dead.empty(), "completed use needs no CPU-frame delay");
    } else if (!std::strcmp(mode, "out-of-order")) {
        signaled = 40;
        make_retirement([&] { ++a; });
        signaled = 20;
        make_retirement([&] { ++b; });
        gpu_done = 21;
        cpu_frame = 100;
        collect_direct();
        need(a == 0 && b == 1 && st().dead.size() == 1, "only completed out-of-order item");
        gpu_done = 41;
        collect_direct();
        need(a == 1 && b == 1, "remaining item freed");
    } else if (!std::strcmp(mode, "idle-completed")) {
        make_retirement([&] { ++a; });
        st().holding = true;
        st().asked = cpu_frame;
        gpu_done = 21;
        collect_idle();
        need(a == 1 && st().dead.empty(), "idle drains without another producer");
    } else if (!std::strcmp(mode, "no-timeline")) {
        timeline = 0;
        make_retirement([&] { ++a; });
        gpu_done = UINT64_MAX;
        cpu_frame = 1000;
        collect_direct();
        need(a == 0 && st().dead.size() == 1, "no-timeline sentinel retains");
    } else if (!std::strcmp(mode, "overflow")) {
        signaled = UINT64_MAX;
        make_retirement([&] { ++a; });
        gpu_done = UINT64_MAX;
        cpu_frame = 1000;
        collect_direct();
        need(a == 0 && st().dead.size() == 1, "overflow sentinel retains");
    } else if (!std::strcmp(mode, "query-failure")) {
        make_retirement([&] { ++a; });
        gpu_done = 0;
        cpu_frame = 1000;
        collect_direct();
        need(a == 0, "failed query cannot release");
    } else if (!std::strcmp(mode, "issued-not-completed")) {
        make_retirement([&] { ++a; });
        signaled = 40;
        cpu_frame = 100;
        collect_direct();
        need(a == 0, "issued or batched signal is not completed");
        gpu_done = 21;
        collect_direct();
        need(a == 1, "old covering signal completed");
    } else if (!std::strcmp(mode, "capture-lifetime")) {
        auto obj = std::shared_ptr<int>(new int(7), [&](int *p) {
            ++a;
            delete p;
        });
        make_retirement([obj] {});
        obj.reset();
        cpu_frame = 100;
        collect_direct();
        need(a == 0, "captured context lives until completion");
        gpu_done = 21;
        collect_direct();
        need(a == 1, "captured context freed exactly once");
    } else if (!std::strcmp(mode, "exact-once")) {
        make_retirement([&] { ++a; });
        cpu_frame = 100;
        gpu_done = 21;
        for (unsigned i = 0; i < 100; i++)
            collect_direct();
        need(a == 1 && st().dead.empty(), "exact-once callback");
    } else if (!std::strcmp(mode, "repeated")) {
        for (unsigned i = 0; i < 500; i++) {
            make_retirement([&] { ++a; });
            gpu_done = ++signaled;
            cpu_frame += 5;
            collect_direct();
            need(a == i + 1 && st().dead.empty(), "repeated retire/reclaim stays bounded");
        }
    } else if (!std::strcmp(mode, "mutex-idle")) {
        make_retirement([&] { ++a; });
        st().holding = true;
        gpu_done = 21;
        cpu_frame = 1000;
        std::promise<void> attempted;
        auto ready = attempted.get_future();
        std::unique_lock held(st().mu);
        auto worker = std::async(std::launch::async, [&] {
            attempted.set_value();
            collect_idle();
        });
        ready.wait();
        need(worker.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
             "collector serializes on existing state mutex");
        held.unlock();
        need(worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
             "collector proceeds after mutex release");
        worker.get();
        need(a == 1, "collector frees once after lock");
    }
#ifdef TEMPORAL_TEST
    else if (!std::strcmp(mode, "keep-selected")) {
        make_retirement([&] { ++a; });
        st().holding = true;
        st().drawing = &engine;
        st().asked = 0;
        selected = true;
        owned = true;
        cpu_frame = 1000;
        gpu_done = 21;
        collect_idle();
        need(a == 1 && st().dead.empty(), "selected engine still drains old resources");
        need(st().drawing == &engine && owned && released_engine == 0 && drop_calls == 0,
             "selected live engine is preserved");
    } else if (!std::strcmp(mode, "keep-cooldown")) {
        make_retirement([&] { ++a; });
        st().holding = true;
        st().drawing = &engine;
        st().asked = 1;
        owned = true;
        cpu_frame = 50;
        gpu_done = 21;
        collect_idle();
        need(a == 1, "cooldown drains old resources");
        need(st().drawing == &engine && owned && !released_engine && !drop_calls, "120-frame live cache is preserved");
    } else if (!std::strcmp(mode, "leave-engine")) {
        st().holding = true;
        st().drawing = &engine;
        st().asked = 1;
        owned = true;
        cpu_frame = 300;
        collect_idle();
        need(!st().drawing && !owned && released_engine == 1 && st().dead.size() == 2 && !freed_engine && !freed_owned,
             "off engine retires but waits for GPU");
        gpu_done = 21;
        cpu_frame = 301;
        collect_idle();
        need(freed_engine == 1 && freed_owned == 1 && st().dead.empty() && !st().holding,
             "idle after completion drains both captures");
        collect_idle();
        need(released_engine == 1 && freed_engine == 1 && freed_owned == 1, "idle after release does not free twice");
    }
#endif
    else
        need(false, "unknown case");
    std::printf("PASS %s\n", mode);
    return 0;
}