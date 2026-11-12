// Source-extracted frame-generation image lifetime, queue ownership and GPU-drain checks.
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <future>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>
constexpr unsigned kMaxMult = 8;
unsigned checks = 0;
void need(bool ok, const char *text) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL %s\n", text);
        std::exit(2);
    }
}
struct TrackingMutex {
    std::mutex mu;
    std::atomic<bool> held{false};
    void lock() {
        mu.lock();
        held = true;
    }
    void unlock() {
        held = false;
        mu.unlock();
    }
};
struct Img {
    unsigned image = 0, view = 0, alloc = 0;
};
struct OutSet {
    Img frame[kMaxMult];
    bool busy = false;
};
struct State {
    uint32_t w = 0, h = 0;
    uint64_t idle_frame = 0;
    Img frames[2], lumas[2], flows[2];
    OutSet sets[3];
    TrackingMutex mu;
    std::deque<unsigned> jobs;
    unsigned mult = 0;
    bool have_prev = false;
    OutSet *recorded = nullptr;
} fg;
struct {
    TrackingMutex present_mu, present_queue_mu, presenter_queue_mu;
    unsigned device = 1;
} dev;
uint64_t frame_no = 1;
bool enabled = false, gpu_pending = true;
unsigned waits = 0, drops = 0, views = 0, logs = 0, serial = 0;
std::vector<unsigned> order;
bool framegen_on() { return enabled; }
uint64_t texture_frame_no() { return frame_no; }
void logf(const char *, ...) { ++logs; }
using VkResult = int;
constexpr VkResult VK_SUCCESS = 0, VK_ERROR_OUT_OF_HOST_MEMORY = -1, VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
                   VK_ERROR_DEVICE_LOST = -4, VK_ERROR_UNKNOWN = -13;
VkResult wait_result = VK_SUCCESS;
unsigned flushes = 0;
void queue_flush_locked() {
    need(dev.present_mu.held, "flush under presentation mutex");
    ++flushes;
}
VkResult vkDeviceWaitIdle(unsigned device) {
    need(device == dev.device, "wait on the owned device");
    need(dev.present_mu.held && dev.present_queue_mu.held && dev.presenter_queue_mu.held,
         "device wait synchronizes every queue");
    need(!fg.mu.held, "do not wait GPU under job queue mutex");
    need(flushes == waits + 1, "held submissions flushed before each device wait");
    ++waits;
    if (wait_result == VK_SUCCESS || wait_result == VK_ERROR_DEVICE_LOST) gpu_pending = false;
    order.push_back(1);
    return wait_result;
}
// WAIT_PRODUCTION
void vkDestroyImageView(unsigned, unsigned, void *) {
    need(!gpu_pending, "view cannot be destroyed with GPU work pending");
    ++views;
}
void destroy_image(unsigned im, unsigned alloc) {
    need(im == alloc, "exact allocation identity");
    need(!gpu_pending, "image cannot be destroyed with GPU work pending");
    ++drops;
    order.push_back(2);
}
// DROP_PRODUCTION
// IDLE_PRODUCTION
void populate() {
    need(fg.w == 0, "prior size released before rebuilding");
    fg.w = 1280;
    fg.h = 720;
    fg.mult = 8;
    fg.have_prev = true;
    fg.recorded = &fg.sets[0];
    gpu_pending = true;
    auto make = [](Img &im) {
        ++serial;
        im = {serial, serial, serial};
    };
    for (unsigned i = 0; i < 2; i++) {
        make(fg.frames[i]);
        make(fg.lumas[i]);
        make(fg.flows[i]);
    }
    for (auto &s : fg.sets)
        for (auto &im : s.frame)
            make(im);
}
unsigned live() {
    unsigned n = 0;
    for (unsigned i = 0; i < 2; i++)
        n += !!fg.frames[i].image + !!fg.lumas[i].image + !!fg.flows[i].image;
    for (auto &s : fg.sets)
        for (auto &im : s.frame)
            n += !!im.image;
    return n;
}
void go_off() {
    frame_no = 1;
    enabled = false;
    framegen_idle();
}
void after_cooldown() {
    frame_no = 122;
    framegen_idle();
}
int main(int argc, char **argv) {
    need(argc == 2, "one case");
    const char *c = argv[1];
    if (!std::strcmp(c, "empty")) {
        frame_no = 1000;
        framegen_idle();
        need(!waits && !drops && !logs, "empty queue does not wait or log");
    } else if (!std::strcmp(c, "active")) {
        populate();
        enabled = true;
        fg.idle_frame = 1;
        frame_no = 1000;
        framegen_idle();
        need(live() == 30 && !waits && !drops && !fg.idle_frame,
             "enabled interpolation keeps all images and resets idle age");
    } else if (!std::strcmp(c, "cooldown")) {
        populate();
        go_off();
        frame_no = 121;
        framegen_idle();
        need(live() == 30 && !waits && !drops, "120-frame idle cooldown retains images");
    } else if (!std::strcmp(c, "off-releases")) {
        populate();
        go_off();
        after_cooldown();
        need(live() == 0 && waits == 1 && drops == 30 && views == 30 && logs == 1,
             "Off releases all size-dependent image bundles");
        need(!fg.w && !fg.h && !fg.mult && !fg.have_prev && !fg.recorded, "released size/history metadata reset");
        need(order.front() == 1, "GPU drain precedes destruction");
    } else if (!std::strcmp(c, "once")) {
        populate();
        go_off();
        after_cooldown();
        for (frame_no = 123; frame_no < 500; frame_no++)
            framegen_idle();
        need(waits == 1 && drops == 30 && logs == 1, "idle cleanup happens only once");
    } else if (!std::strcmp(c, "pending-job")) {
        populate();
        go_off();
        fg.jobs.push_back(1);
        after_cooldown();
        need(live() == 30 && !waits && !drops, "queued presentation job retains images");
        fg.jobs.clear();
        framegen_idle();
        need(live() == 0 && waits == 1, "release once queued presentation job has finished");
    } else if (!std::strcmp(c, "busy-set")) {
        populate();
        go_off();
        fg.sets[1].busy = true;
        after_cooldown();
        need(live() == 30 && !waits && !drops, "dequeued job with busy set retains images");
        fg.sets[1].busy = false;
        framegen_idle();
        need(live() == 0 && waits == 1, "release only after displayed set is idle");
    } else if (!std::strcmp(c, "gpu-pending")) {
        populate();
        go_off();
        after_cooldown();
        need(!gpu_pending && waits == 1 && drops == 30, "CPU idle alone is insufficient: GPU barrier precedes free");
    } else if (!std::strcmp(c, "re-enable")) {
        populate();
        go_off();
        enabled = true;
        frame_no = 80;
        framegen_idle();
        need(!fg.idle_frame && live() == 30, "re-enable keeps images");
        enabled = false;
        frame_no = 81;
        framegen_idle();
        frame_no = 122;
        framegen_idle();
        need(live() == 30 && !waits, "new Off transition restarts cooldown");
        frame_no = 202;
        framegen_idle();
        need(live() == 0 && waits == 1, "new full cooldown permits cleanup");
    } else if (!std::strcmp(c, "frame-rewind")) {
        populate();
        enabled = false;
        fg.idle_frame = 200;
        frame_no = 100;
        framegen_idle();
        need(live() == 30 && !waits && fg.idle_frame == 100, "frame rewind cannot underflow age");
        frame_no = 221;
        framegen_idle();
        need(live() == 0, "rewound cooldown eventually releases");
    } else if (!std::strcmp(c, "rebuild")) {
        populate();
        go_off();
        after_cooldown();
        need(live() == 0, "first disable releases");
        populate();
        enabled = true;
        frame_no = 123;
        framegen_idle();
        need(live() == 30 && waits == 1, "recreated generation remains live");
        enabled = false;
        frame_no = 124;
        framegen_idle();
        frame_no = 245;
        framegen_idle();
        need(live() == 0 && waits == 2 && drops == 60 && views == 60, "second generation releases once");
    } else if (!std::strcmp(c, "null-images")) {
        fg.w = 1280;
        fg.h = 720;
        fg.frames[0] = {1, 1, 1};
        fg.mult = 2;
        go_off();
        after_cooldown();
        need(!fg.w && drops == 1 && views == 1, "partially populated size is safe");
    } else if (!std::strcmp(c, "queue-mutex")) {
        populate();
        go_off();
        frame_no = 122;
        std::promise<void> entered;
        auto ready = entered.get_future();
        std::unique_lock held(fg.mu);
        auto worker = std::async(std::launch::async, [&] {
            entered.set_value();
            framegen_idle();
        });
        ready.wait();
        need(worker.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
             "queue state inspection uses its mutex");
        held.unlock();
        need(worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
             "cleanup proceeds after mutex unlock");
        worker.get();
        need(waits == 1 && drops == 30, "locked cleanup completed");
    } else if (!std::strcmp(c, "host-oom") || !std::strcmp(c, "device-oom") ||
               !std::strcmp(c, "unknown-error") || !std::strcmp(c, "failure-reenable")) {
        populate();
        go_off();
        wait_result = !std::strcmp(c, "device-oom") ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                    : !std::strcmp(c, "unknown-error") ? VK_ERROR_UNKNOWN : VK_ERROR_OUT_OF_HOST_MEMORY;
        after_cooldown();
        need(live() == 30 && gpu_pending && waits == 1 && !drops && !views,
             "failed wait cannot prove completion or release any image or view");
        need(!dev.present_mu.held && !dev.present_queue_mu.held && !dev.presenter_queue_mu.held,
             "failed wait releases all queue locks");
        if (!std::strcmp(c, "failure-reenable")) {
            enabled = true;
            frame_no = 123;
            framegen_idle();
            need(live() == 30 && waits == 1 && !fg.idle_frame, "re-enable after failure retains usable images");
            enabled = false;
            frame_no = 124;
            framegen_idle();
            frame_no = 245;
        } else {
            for (frame_no = 123; frame_no <= 242; ++frame_no) framegen_idle();
            need(live() == 30 && waits == 1, "failed wait is not retried every present");
            frame_no = 243;
        }
        wait_result = VK_SUCCESS;
        framegen_idle();
        need(live() == 0 && drops == 30 && views == 30 && waits == 2,
             "confirmed completion after failure releases each resource exactly once");
        framegen_idle();
        need(drops == 30 && waits == 2, "no repeat release after successful retry");
    } else if (!std::strcmp(c, "device-lost")) {
        populate();
        go_off();
        wait_result = VK_ERROR_DEVICE_LOST;
        after_cooldown();
        // Vulkan permits destruction after device loss. Conservatively retaining
        // these images for the renderer's existing device-loss exit is safe too.
        need(waits == 1 && (live() == 30 || live() == 0), "device loss has no partial destruction");
        need(!dev.present_mu.held && !dev.present_queue_mu.held && !dev.presenter_queue_mu.held,
             "device loss releases queue locks");
    } else
        need(false, "unknown case");
    std::printf("PASS %s (%u checks)\n", c, checks);
    return 0;
}