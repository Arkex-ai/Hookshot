#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum Cache { None = -1, Owner, Failure, FragmentWords };
int fail_cache = None;
unsigned allocation_faults = 0;
template<class T, int Tag> struct FaultAllocator : std::allocator<T> {
    using value_type = T;
    FaultAllocator() = default;
    template<class U> FaultAllocator(const FaultAllocator<U, Tag>&) {}
    template<class U> struct rebind { using other = FaultAllocator<U, Tag>; };
    T* allocate(size_t n) {
        if (fail_cache == Tag) { fail_cache = None; allocation_faults++; throw std::bad_alloc(); }
        return std::allocator<T>{}.allocate(n);
    }
};
template<class K, class V, int Tag> using FaultMap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, FaultAllocator<std::pair<const K, V>, Tag>>;
template<class K, int Tag> using FaultSet = std::unordered_set<K, std::hash<K>, std::equal_to<K>, FaultAllocator<K, Tag>>;
template<class T, int Tag> using FaultVector = std::vector<T, FaultAllocator<T, Tag>>;
struct { VkDevice device{}; } dev;
struct Targets {
    int n = 2;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkFormat color_format[8]{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT};
    void* depth = nullptr;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
};
uint64_t mix(uint64_t a, uint64_t b) { return (a ^ b) * 0x9e3779b97f4a7c15ull; }
bool depth_aspect(VkFormat f) { return f != VK_FORMAT_UNDEFINED; }
bool has_stencil(VkFormat f) { return f == VK_FORMAT_D24_UNORM_S8_UINT; }
// Authored words model the type tag and Location decoration, not a driver-executable shader.
const uint32_t CLEAR_VERT_SPV[]{0x07230203, 9, 0, 0, 0};
const uint32_t CLEAR_F_FRAG_SPV[]{0x07230203, 0, 0, 0, 0, (4u << 16) | 71u, 1, 30, 0};
const uint32_t CLEAR_U_FRAG_SPV[]{0x07230203, 1, 0, 0, 0, (4u << 16) | 71u, 1, 30, 0};
const uint32_t CLEAR_I_FRAG_SPV[]{0x07230203, 2, 0, 0, 0, (4u << 16) | 71u, 1, 30, 0};
FaultMap<uint64_t, VkPipeline, Owner> clear_pipes;
VkPipelineLayout clear_layout = VK_NULL_HANDLE;
VkPipelineCache pcache = VK_NULL_HANDLE;
struct PlayCache {
    VkPipelineCache cache = reinterpret_cast<VkPipelineCache>(uintptr_t(0x1234));
    std::atomic<unsigned> users{0}, built{0}, compiled{0}, from_driver{0}, from_start{0};
};
std::atomic<PlayCache*> play_cache{nullptr};
unsigned guarded_calls = 0;
bool cache_miss = false;
std::vector<VkPipelineCache> queried_caches;

std::unordered_set<VkPipelineLayout> layouts;
std::unordered_map<VkShaderModule, std::vector<uint32_t>> modules;
std::unordered_set<VkPipeline> pipelines;
uintptr_t serial = 100;
template<class T> T handle() { return reinterpret_cast<T>(++serial); }
uint64_t frame_no = 1;
uint64_t texture_frame_no() { return frame_no; }
std::string fault;
bool garbage = true, arm_owner = false, arm_metadata = false;
VkResult error_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
unsigned layout_calls = 0, shader_calls = 0, pipeline_calls = 0, pipeline_destroys = 0;
Targets expected;
int expected_index = 1, expected_kind = 1;
uint32_t expected_mask = 5;
std::mutex replay_mu;
VkResult fail_result() {
    if (arm_metadata) { arm_metadata = false; fail_cache = Failure; }
    return error_result;
}
VkResult vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo* ci, const VkAllocationCallbacks*, VkPipelineLayout* out) {
    layout_calls++;
    assert(ci->pushConstantRangeCount == 1 && ci->pPushConstantRanges[0].stageFlags == VK_SHADER_STAGE_FRAGMENT_BIT);
    assert(ci->pPushConstantRanges[0].offset == 0 && ci->pPushConstantRanges[0].size == 16);
    if (fault == "layout") {
        *out = garbage ? reinterpret_cast<VkPipelineLayout>(uintptr_t(0xdead)) : VK_NULL_HANDLE;
        return fail_result();
    }
    *out = handle<VkPipelineLayout>(); layouts.insert(*out); return VK_SUCCESS;
}
VkResult vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks*, VkShaderModule* out) {
    shader_calls++;
    const bool vertex = ci->codeSize == sizeof CLEAR_VERT_SPV;
    if ((fault == "vertex" && vertex) || (fault == "fragment" && !vertex)) {
        *out = garbage ? reinterpret_cast<VkShaderModule>(uintptr_t(0xdead)) : VK_NULL_HANDLE;
        return fail_result();
    }
    *out = handle<VkShaderModule>();
    modules.emplace(*out, std::vector<uint32_t>(ci->pCode, ci->pCode + ci->codeSize / 4));
    return VK_SUCCESS;
}
VkResult vkCreateGraphicsPipelines(VkDevice, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks*, VkPipeline* out) {
    pipeline_calls++;
    queried_caches.push_back(cache);
    assert(n == 1 && layouts.count(ci->layout) && ci->stageCount == 2);
    assert(ci->pStages[0].stage == VK_SHADER_STAGE_VERTEX_BIT && ci->pStages[1].stage == VK_SHADER_STAGE_FRAGMENT_BIT);
    for (unsigned i = 0; i < 2; i++) assert(modules.count(ci->pStages[i].module));
    const auto& vert = modules.at(ci->pStages[0].module);
    const auto& frag = modules.at(ci->pStages[1].module);
    assert(vert == std::vector<uint32_t>(std::begin(CLEAR_VERT_SPV), std::end(CLEAR_VERT_SPV)));
    assert(frag.size() == 9 && frag[1] == (uint32_t)expected_kind && frag[8] == (uint32_t)expected_index);
    assert(ci->pColorBlendState->attachmentCount == (uint32_t)expected.n);
    for (int i = 0; i < expected.n; i++) assert(ci->pColorBlendState->pAttachments[i].colorWriteMask == (i == expected_index ? expected_mask : 0));
    const auto* ri = static_cast<const VkPipelineRenderingCreateInfo*>(ci->pNext);
    assert(ri->colorAttachmentCount == (uint32_t)expected.n);
    for (int i = 0; i < expected.n; i++) assert(ri->pColorAttachmentFormats[i] == expected.color_format[i]);
    assert(ri->depthAttachmentFormat == (expected.depth ? expected.depth_format : VK_FORMAT_UNDEFINED));
    assert(ri->stencilAttachmentFormat == (expected.depth && has_stencil(expected.depth_format) ? expected.depth_format : VK_FORMAT_UNDEFINED));
    assert(ci->pMultisampleState->rasterizationSamples == expected.samples);
    assert(ci->pInputAssemblyState->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    assert(ci->pDynamicState->dynamicStateCount == 2 && ci->pRasterizationState->cullMode == VK_CULL_MODE_NONE);
    if (cache_miss && (ci->flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT)) { *out = VK_NULL_HANDLE; return VK_PIPELINE_COMPILE_REQUIRED; }
    if (fault == "pipeline") { *out = VK_NULL_HANDLE; return fail_result(); }
    *out = handle<VkPipeline>(); pipelines.insert(*out);
    // Target owning insertion after the Vulkan resource exists, independent of STL allocation order.
    if (arm_owner) { arm_owner = false; fail_cache = Owner; }
    return VK_SUCCESS;
}
void vkDestroyShaderModule(VkDevice, VkShaderModule m, const VkAllocationCallbacks*) { if (m) assert(modules.erase(m) == 1); }
void vkDestroyPipeline(VkDevice, VkPipeline p, const VkAllocationCallbacks*) { if (p) { assert(pipelines.erase(p) == 1); pipeline_destroys++; } }
VkResult guarded_graphics(VkPipelineCache cache, const VkGraphicsPipelineCreateInfo* ci, VkPipeline* out) {
    guarded_calls++;
    return vkCreateGraphicsPipelines(dev.device, cache, 1, ci, nullptr, out);
}
/* ACTUAL */

VkPipeline get(const Targets& at, int index = 1, uint32_t mask = 5, int kind = 1) {
    std::lock_guard guard(replay_mu);   // Model the actual clear caller's replay-lock boundary.
    expected = at; expected_index = index; expected_mask = mask; expected_kind = kind;
    return clear_pipeline(at, index, mask, kind);
}
void valid(VkPipeline p) { assert(p && pipelines.count(p) && modules.empty()); }

int64_t st_masked_clears = 0;
bool last_dyn_valid = true, last_desc_valid = true, last_vb_valid = true;
VkPipeline last_pipeline = VK_NULL_HANDLE;
struct { bool on = true; } vertex_set;
bool disturbed = false;
unsigned binds = 0, draws = 0, viewports = 0, scissors = 0, pushes = 0;
void descriptors_disturbed() { disturbed = true; }
void vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline p) { valid(p); binds++; }
void vkCmdSetViewport(VkCommandBuffer, uint32_t, uint32_t n, const VkViewport* v) { assert(n == 1 && v->width == 3440 && v->height == 1440); viewports++; }
void vkCmdSetScissor(VkCommandBuffer, uint32_t, uint32_t n, const VkRect2D* r) { assert(n == 1 && r->extent.width == 3440 && r->extent.height == 1440); scissors++; }
void vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout layout, VkShaderStageFlags, uint32_t offset, uint32_t n, const void*) { assert(layouts.count(layout) && offset == 0 && n == 16); pushes++; }
void vkCmdDraw(VkCommandBuffer, uint32_t vertices, uint32_t instances, uint32_t first, uint32_t base) { assert(binds && vertices == 3 && instances == 1 && !first && !base); draws++; }
void consume(const Targets& at, int index = 1, uint32_t mask = 5, int kind = 1) {
    std::lock_guard guard(replay_mu);
    expected = at; expected_index = index; expected_mask = mask; expected_kind = kind;
    const auto cmd = reinterpret_cast<VkCommandBuffer>(uintptr_t(30));
    const uint32_t lw = 3440, lh = 1440;
    const VkRect2D rect{{0, 0}, {lw, lh}};
    const float rgba[4]{0.1f, 0.2f, 0.3f, 1};
    /* CONSUMER */
}

int main(int argc, char** argv) {
    struct Routes { ~Routes() { assert(guarded_calls == pipeline_calls); } } routes;

    assert(argc == 2);
    const std::string test = argv[1];
    Targets at;
    if (test == "concurrent") {
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; i++) workers.emplace_back([&] { for (int j = 0; j < 50; j++) valid(get(at)); });
        for (auto& worker : workers) worker.join();
        assert(pipeline_calls == 1 && shader_calls == 2 && layout_calls == 1); return 0;
    }
    if (test == "consumer" || test == "consumer-cached") {
        if (test == "consumer") {
            fault = "pipeline"; consume(at);
            assert(!binds && !draws && !viewports && !scissors && !pushes && !disturbed && !st_masked_clears);
            fault.clear(); frame_no++;
        } else valid(get(at));
        consume(at);
        assert(binds == 1 && draws == 1 && viewports == 1 && scissors == 1 && pushes == 1 && st_masked_clears == 1);
        assert(disturbed && !last_dyn_valid && !last_desc_valid && !last_vb_valid && !vertex_set.on); return 0;
    }
    if (test == "independent" || test == "valid-after-failure" || test == "failure-metadata") {
        VkPipeline first = VK_NULL_HANDLE;
        if (test != "independent") first = get(at, 0), valid(first);
        fault = "pipeline"; arm_metadata = test == "failure-metadata";
        assert(!get(at)); fault.clear();
        if (first) assert(get(at, 0) == first);
        Targets other = at; other.n = 3; other.color_format[2] = VK_FORMAT_R8G8B8A8_UNORM;
        if (test == "failure-metadata") { assert(!get(other, 2) && allocation_faults == 1); }
        else valid(get(other, 2));
        assert(!get(at)); frame_no++; valid(get(at));
        assert(!pipeline_destroys); return 0;
    }
    if (test == "layout" || test == "vertex" || test == "fragment" || test == "pipeline" ||
        test == "layout-null" || test == "vertex-null" || test == "fragment-null" ||
        test == "host-oom" || test == "unknown" || test == "owner-commit" || test == "fragment-alloc") {
        fault = test;
        if (test.find("-null") != std::string::npos) fault = test.substr(0, test.find('-')), garbage = false;
        if (test == "host-oom" || test == "unknown") fault = "pipeline", error_result = test == "host-oom" ? VK_ERROR_OUT_OF_HOST_MEMORY : VK_ERROR_UNKNOWN;
        if (test == "owner-commit") fault.clear(), arm_owner = true;
        if (test == "fragment-alloc") fault.clear(), fail_cache = FragmentWords;
        assert(!get(at) && clear_pipes.empty() && modules.empty() && pipelines.empty());
        const auto lc = layout_calls, sc = shader_calls, pc = pipeline_calls;
        assert(!get(at) && layout_calls == lc && shader_calls == sc && pipeline_calls == pc);
        assert(pipeline_destroys == (test == "owner-commit" ? 1u : 0u));
        if (test == "owner-commit" || test == "fragment-alloc") assert(allocation_faults == 1);
        fault.clear(); assert(!get(at)); frame_no++; valid(get(at));
        assert(layouts.size() == 1); return 0;
    }
    int index = 1, kind = 1;
    if (test == "float") kind = 0, at.color_format[1] = VK_FORMAT_R8G8B8A8_UNORM;
    if (test == "signed") kind = 2, at.color_format[1] = VK_FORMAT_R32_SINT;
    if (test == "index7") {
        at.n = 8; index = 7;
        std::fill(at.color_format + 2, at.color_format + 8, VK_FORMAT_R32_UINT);
    }
    if (test == "msaa") at.samples = VK_SAMPLE_COUNT_4_BIT;
    if (test == "depth" || test == "stencil") at.depth = &at, at.depth_format = test == "depth" ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D24_UNORM_S8_UINT;
    if (test == "play-cache-hit" || test == "play-cache-miss") {
        PlayCache active;
        play_cache.store(&active);
        cache_miss = test == "play-cache-miss";
        auto first = get(at, index, 5, kind); valid(first);
        const unsigned calls = cache_miss ? 3 : 1;
        assert(pipeline_calls == calls && shader_calls == 2 && layout_calls == 1);
        assert(active.users == 0 && active.built == 1);
        assert(active.compiled == (cache_miss ? 1u : 0u));
        assert(queried_caches.front() == active.cache && queried_caches.back() == active.cache);
        if (cache_miss) assert(queried_caches[1] == pcache);
        assert(get(at, index, 5, kind) == first && pipeline_calls == calls);
        play_cache.store(nullptr);
        return 0;
    }
    auto first = get(at, index, 5, kind); valid(first);
    assert(get(at, index, 5, kind) == first && pipeline_calls == 1 && shader_calls == 2 && layout_calls == 1);
    if (test == "distinct-mask") valid(get(at, index, 3, kind));
    if (test == "distinct-format") { at.color_format[0] = VK_FORMAT_R16G16B16A16_SFLOAT; valid(get(at, index, 5, kind)); }
    if (test == "distinct-samples") { at.samples = VK_SAMPLE_COUNT_4_BIT; valid(get(at, index, 5, kind)); }
    if (test == "distinct-depth") { at.depth = &at; at.depth_format = VK_FORMAT_D32_SFLOAT; valid(get(at, index, 5, kind)); }
    if (test == "distinct-index") valid(get(at, 0, 5, kind));
    if (test == "signed-after-unsigned") { at.color_format[index] = VK_FORMAT_R32_SINT; valid(get(at, index, 5, 2)); }
    if (test.find("distinct-") == 0) assert(pipeline_calls == 2 && layouts.size() == 1 && pipelines.size() == 2);
    if (test == "signed-after-unsigned") assert(pipeline_calls == 2 && layouts.size() == 1 && pipelines.size() == 2);
    return 0;
}
