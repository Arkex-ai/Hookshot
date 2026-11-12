// SPDX-License-Identifier: GPL-3.0-or-later
// Full shipped ASTC shader against a pinned GPL baseline, using authored blocks only.
// Loader/validation scaffold follows tests/vulkan_copy_span_gpu.cpp.
// No game, dump, save, encoder download or shader cache is read.
#define VK_NO_PROTOTYPES
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <vulkan/vulkan_core.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x);                                    \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)
#define APIS(X)                                                                                    \
    X(vkCreateInstance)                                                                            \
    X(vkDestroyInstance) X(vkEnumerateInstanceLayerProperties)                                     \
        X(vkEnumerateInstanceExtensionProperties) X(vkEnumeratePhysicalDevices)                    \
            X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFormatProperties)                \
                X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
                    X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateBuffer) X(  \
                        vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)    \
                        X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(      \
                            vkBindImageMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory)  \
                            X(vkUnmapMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool)        \
                                X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)                \
                                    X(vkEndCommandBuffer) X(vkCmdPipelineBarrier)                  \
                                        X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer)        \
                                            X(vkCmdFillBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) \
                                                X(vkCmdResolveImage) X(vkCmdClearColorImage)       \
                                                    X(vkFreeCommandBuffers) X(vkCreateFence)       \
                                                        X(vkDestroyFence) X(vkQueueSubmit)         \
                                                            X(vkWaitForFences)
#define EXTRA_APIS(X)                                                                              \
    X(vkGetPhysicalDeviceFeatures)                                                                 \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateImageView) X(vkDestroyImageView)    \
        X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout)   \
            X(vkDestroyPipelineLayout) X(vkCreateComputePipelines) X(vkDestroyPipeline)            \
                X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets)   \
                    X(vkUpdateDescriptorSets) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets)      \
                        X(vkCmdPushConstants) X(vkCmdDispatch)
#define DECLARE(n) PFN_##n n;
APIS(DECLARE)
EXTRA_APIS(DECLARE)

std::atomic<int> validation_errors{0};
uint32_t memory_type(const VkPhysicalDeviceMemoryProperties &props, uint32_t bits,
                     VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required)
            return i;
    return UINT32_MAX;
}
int skip(const char *reason) {
    std::printf("SKIP: %s\n", reason);
    return 77;
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

struct Buffer {
    VkBuffer b{};
    VkDeviceMemory m{};
    void *ptr{};
};
Buffer make_buffer(VkDevice d, const VkPhysicalDeviceMemoryProperties &mem, uint64_t bytes,
                   VkBufferUsageFlags usage) {
    Buffer o;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = bytes;
    ci.usage = usage;
    CHECK(vkCreateBuffer(d, &ci, nullptr, &o.b) == VK_SUCCESS);
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(d, o.b, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        memory_type(mem, req.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(ai.memoryTypeIndex != UINT32_MAX);
    CHECK(vkAllocateMemory(d, &ai, nullptr, &o.m) == VK_SUCCESS);
    CHECK(vkBindBufferMemory(d, o.b, o.m, 0) == VK_SUCCESS);
    CHECK(vkMapMemory(d, o.m, 0, bytes, 0, &o.ptr) == VK_SUCCESS);
    return o;
}
void drop(VkDevice d, Buffer o) {
    vkUnmapMemory(d, o.m);
    vkDestroyBuffer(d, o.b, nullptr);
    vkFreeMemory(d, o.m, nullptr);
}
std::vector<uint32_t> shader(const char *path) {
    FILE *f = std::fopen(path, "rb");
    CHECK(f);
    CHECK(!std::fseek(f, 0, SEEK_END));
    long n = std::ftell(f);
    CHECK(n > 0 && n % 4 == 0);
    std::rewind(f);
    std::vector<uint32_t> words((size_t)n / 4);
    CHECK(std::fread(words.data(), 1, (size_t)n, f) == (size_t)n);
    CHECK(!std::fclose(f));
    return words;
}
uint32_t rng(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4);
    const char *requested_device = argc == 4 ? argv[3] : nullptr;
#ifdef _WIN32
    auto loader = LoadLibraryW(L"vulkan-1.dll");
    auto symbol = [&](const char *name) {
        return loader ? (void *)GetProcAddress(loader, name) : nullptr;
    };
#else
    auto loader = dlopen("libvulkan.so.1", RTLD_NOW);
    auto symbol = [&](const char *name) { return loader ? dlsym(loader, name) : nullptr; };
#endif
    if (!loader)
        return skip("Vulkan loader unavailable");
    bool api_ready = true;
#define LOAD(n)                                                                                    \
    n = reinterpret_cast<PFN_##n>(symbol(#n));                                                     \
    api_ready = api_ready && n;
    APIS(LOAD)
    EXTRA_APIS(LOAD)
#undef LOAD
    auto gip = reinterpret_cast<PFN_vkGetInstanceProcAddr>(symbol("vkGetInstanceProcAddr"));
    if (!api_ready || !gip) {
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return skip("required Vulkan entry points unavailable");
    }
    uint32_t nl = 0;
    CHECK(vkEnumerateInstanceLayerProperties(&nl, nullptr) == VK_SUCCESS);
    std::vector<VkLayerProperties> layers_list(nl);
    CHECK(vkEnumerateInstanceLayerProperties(&nl, layers_list.data()) == VK_SUCCESS);
    uint32_t ne = 0;
    CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &ne, nullptr) == VK_SUCCESS);
    std::vector<VkExtensionProperties> extensions(ne);
    CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &ne, extensions.data()) == VK_SUCCESS);
    bool validation = false, debug_utils = false;
    for (const auto &layer : layers_list)
        if (!std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation"))
            validation = true;
    for (const auto &ext : extensions)
        if (!std::strcmp(ext.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
            debug_utils = true;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT db{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    db.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    db.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    db.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                            VkDebugUtilsMessageTypeFlagsEXT,
                            const VkDebugUtilsMessengerCallbackDataEXT *data, void *) -> VkBool32 {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            validation_errors.fetch_add(1, std::memory_order_relaxed);
            std::fprintf(stderr, "validation: %s\n", data->pMessage);
        }
        return VK_FALSE;
    };
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (validation) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer;
    }
    if (validation && debug_utils) {
        ici.enabledExtensionCount = 1;
        ici.ppEnabledExtensionNames = &extension;
        ici.pNext = &db;
    }
    VkInstance instance{};
    const VkResult instance_result = vkCreateInstance(&ici, nullptr, &instance);
    if (instance_result == VK_ERROR_INCOMPATIBLE_DRIVER ||
        instance_result == VK_ERROR_INITIALIZATION_FAILED) {
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return skip("Vulkan instance or compatible driver unavailable");
    }
    CHECK(instance_result == VK_SUCCESS);
    VkDebugUtilsMessengerEXT messenger{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_debug = nullptr;
    if (validation && debug_utils) {
        auto create_debug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            gip(instance, "vkCreateDebugUtilsMessengerEXT"));
        destroy_debug = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            gip(instance, "vkDestroyDebugUtilsMessengerEXT"));
        CHECK(create_debug && destroy_debug &&
              create_debug(instance, &db, nullptr, &messenger) == VK_SUCCESS);
    }
    std::printf("Khronos validation: %s\n",
                validation ? (debug_utils ? "enabled" : "enabled; debug utils unavailable")
                           : "unavailable");
    uint32_t np = 0;
    const VkResult enumerate_result = vkEnumeratePhysicalDevices(instance, &np, nullptr);
    if (enumerate_result == VK_ERROR_INITIALIZATION_FAILED) {
        if (messenger) {
            destroy_debug(instance, messenger, nullptr);
        }
        vkDestroyInstance(instance, nullptr);
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return skip("Vulkan physical-device enumeration unavailable");
    }
    CHECK(enumerate_result == VK_SUCCESS);
    if (!np) {
        if (messenger)
            destroy_debug(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return skip("no Vulkan physical devices found");
    }
    std::vector<VkPhysicalDevice> devices(np);
    CHECK(vkEnumeratePhysicalDevices(instance, &np, devices.data()) == VK_SUCCESS);
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t family = UINT32_MAX;
    const std::string wanted = requested_device ? lower(requested_device) : std::string{};
    for (auto candidate : devices) {
        VkPhysicalDeviceProperties candidate_properties{};
        vkGetPhysicalDeviceProperties(candidate, &candidate_properties);
        if (candidate_properties.apiVersion < VK_API_VERSION_1_1)
            continue;
        if (!wanted.empty() &&
            lower(candidate_properties.deviceName).find(wanted) == std::string::npos)
            continue;
        if (wanted.empty() && candidate_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
            continue;
        VkPhysicalDeviceFeatures candidate_features{};
        vkGetPhysicalDeviceFeatures(candidate, &candidate_features);
        if (!candidate_features.shaderStorageImageWriteWithoutFormat ||
            !candidate_features.robustBufferAccess)
            continue;
        VkFormatProperties format{};
        vkGetPhysicalDeviceFormatProperties(candidate, VK_FORMAT_R8G8B8A8_UNORM, &format);
        const VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                            VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                            VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ((format.optimalTilingFeatures & needed) != needed)
            continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> queues(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &nq, queues.data());
        uint32_t candidate_family = UINT32_MAX;
        for (uint32_t q = 0; q < nq; ++q)
            if (queues[q].queueCount && (queues[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                candidate_family = q;
                break;
            }
        if (candidate_family == UINT32_MAX)
            continue;
        VkPhysicalDeviceMemoryProperties candidate_memory{};
        vkGetPhysicalDeviceMemoryProperties(candidate, &candidate_memory);
        if (memory_type(candidate_memory, UINT32_MAX,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == UINT32_MAX)
            continue;
        physical = candidate;
        properties = candidate_properties;
        memory = candidate_memory;
        family = candidate_family;
        break;
    }
    if (!physical) {
        if (messenger)
            destroy_debug(instance, messenger, nullptr);
        vkDestroyInstance(instance, nullptr);
#ifdef _WIN32
        FreeLibrary(loader);
#else
        dlclose(loader);
#endif
        return requested_device
                   ? skip("no compatible Vulkan device matched the requested name")
                   : skip("no Vulkan device supports the required queue, format, and host memory");
    }
    std::printf("GPU: %s; Vulkan %u.%u.%u; driver %#x\n", properties.deviceName,
                VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
                VK_VERSION_PATCH(properties.apiVersion), properties.driverVersion);
    float priority = 1;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical, &supported);
    CHECK(supported.shaderStorageImageWriteWithoutFormat);
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabled.robustBufferAccess = VK_TRUE;
    dci.pEnabledFeatures = &enabled;
    VkDevice device{};
    CHECK(vkCreateDevice(physical, &dci, nullptr, &device) == VK_SUCCESS);
    VkQueue queue{};
    vkGetDeviceQueue(device, family, 0, &queue);

    constexpr uint32_t blocks = 32, input_bytes = blocks * blocks * 16;
    constexpr uint64_t capacity = 384ull * 384 * 4;
    Buffer input = make_buffer(device, memory, input_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Buffer readback = make_buffer(device, memory, capacity * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkDescriptorSetLayoutBinding bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 2;
    dlci.pBindings = bindings;
    VkDescriptorSetLayout dl{};
    CHECK(vkCreateDescriptorSetLayout(device, &dlci, nullptr, &dl) == VK_SUCCESS);
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &dl;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    VkPipelineLayout layout{};
    CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &layout) == VK_SUCCESS);
    VkPipeline pipelines[2]{};
    for (unsigned i = 0; i < 2; i++) {
        auto words = shader(argv[i + 1]);
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = words.size() * 4;
        smci.pCode = words.data();
        VkShaderModule module{};
        CHECK(vkCreateShaderModule(device, &smci, nullptr, &module) == VK_SUCCESS);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = layout;
        CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipelines[i]) ==
              VK_SUCCESS);
        vkDestroyShaderModule(device, module, nullptr);
    }
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 2;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    VkDescriptorPool descriptor_pool{};
    CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &descriptor_pool) == VK_SUCCESS);
    VkDescriptorSetLayout layouts[2] = {dl, dl};
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descriptor_pool;
    dai.descriptorSetCount = 2;
    dai.pSetLayouts = layouts;
    VkDescriptorSet sets[2]{};
    CHECK(vkAllocateDescriptorSets(device, &dai, sets) == VK_SUCCESS);
    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = family;
    VkCommandPool pool{};
    CHECK(vkCreateCommandPool(device, &cpci, nullptr, &pool) == VK_SUCCESS);
    const uint32_t footprints[][2] = {{4, 4},  {5, 4},   {5, 5},   {6, 5},  {6, 6},
                                      {8, 5},  {8, 6},   {8, 8},   {10, 5}, {10, 6},
                                      {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    uint64_t compared = 0, nonzero = 0;
    unsigned cases = 0;
    for (auto &footprint : footprints)
        for (unsigned orientation = 0; orientation < (footprint[0] == footprint[1] ? 1u : 2u);
             orientation++)
            for (unsigned native = 0; native < 2; native++)
                for (unsigned clipped = 0; clipped < 2; clipped++) {
                    const uint32_t bw = footprint[orientation], bh = footprint[1 - orientation],
                                   width = bw * blocks - clipped, height = bh * blocks - clipped;
                    uint32_t random =
                        0x97ac123bu ^ (bw << 16) ^ (bh << 8) ^ (native << 4) ^ clipped;
                    std::memset(readback.ptr, 0xa5, capacity * 2);
                    auto words = (uint32_t *)input.ptr;
                    for (unsigned i = 0; i < input_bytes / 4; i++)
                        words[i] = rng(random);
                    // Explicit valid LDR void-extent blocks amongst authored random blocks.
                    for (unsigned i = 0; i < input_bytes / 16; i += 17) {
                        words[i * 4] = 0xfffffdfcu;
                        words[i * 4 + 1] = 0xffffffffu;
                        words[i * 4 + 2] = 0x22221111u;
                        words[i * 4 + 3] = 0xffff4444u;
                    }
                    VkImage images[2]{};
                    VkDeviceMemory allocations[2]{};
                    VkImageView views[2]{};
                    for (unsigned i = 0; i < 2; i++) {
                        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                        ci.imageType = VK_IMAGE_TYPE_2D;
                        ci.format = VK_FORMAT_R8G8B8A8_UNORM;
                        ci.extent = {width, height, 1};
                        ci.mipLevels = 1;
                        ci.arrayLayers = 1;
                        ci.samples = VK_SAMPLE_COUNT_1_BIT;
                        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
                        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                        CHECK(vkCreateImage(device, &ci, nullptr, &images[i]) == VK_SUCCESS);
                        VkMemoryRequirements req{};
                        vkGetImageMemoryRequirements(device, images[i], &req);
                        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                        ai.allocationSize = req.size;
                        ai.memoryTypeIndex = memory_type(memory, req.memoryTypeBits, 0);
                        CHECK(ai.memoryTypeIndex != UINT32_MAX);
                        CHECK(vkAllocateMemory(device, &ai, nullptr, &allocations[i]) ==
                              VK_SUCCESS);
                        CHECK(vkBindImageMemory(device, images[i], allocations[i], 0) ==
                              VK_SUCCESS);
                        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                        vi.image = images[i];
                        vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                        vi.format = ci.format;
                        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        CHECK(vkCreateImageView(device, &vi, nullptr, &views[i]) == VK_SUCCESS);
                        VkDescriptorBufferInfo buffer{input.b, 0, input_bytes};
                        VkDescriptorImageInfo image{VK_NULL_HANDLE, views[i],
                                                    VK_IMAGE_LAYOUT_GENERAL};
                        VkWriteDescriptorSet write[2]{};
                        write[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                        write[0].dstSet = sets[i];
                        write[0].dstBinding = 0;
                        write[0].descriptorCount = 1;
                        write[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                        write[0].pBufferInfo = &buffer;
                        write[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                        write[1].dstSet = sets[i];
                        write[1].dstBinding = 1;
                        write[1].descriptorCount = 1;
                        write[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                        write[1].pImageInfo = &image;
                        vkUpdateDescriptorSets(device, 2, write, 0, nullptr);
                    }
                    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                    cai.commandPool = pool;
                    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    cai.commandBufferCount = 1;
                    VkCommandBuffer cmd{};
                    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd) == VK_SUCCESS);
                    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                    CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
                    for (unsigned i = 0; i < 2; i++) {
                        VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        ib.image = images[i];
                        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                        ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                             nullptr, 1, &ib);
                        uint32_t constants[8] = {bw, bh, input_bytes, 4096, 9, 0, 0, native};
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[i]);
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                                                &sets[i], 0, nullptr);
                        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32,
                                           constants);
                        vkCmdDispatch(cmd, blocks / 8, blocks / 8, 1);
                        ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                        ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                             nullptr, 1, &ib);
                        VkBufferImageCopy copy{};
                        copy.bufferOffset = i * capacity;
                        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                        copy.imageExtent = {width, height, 1};
                        vkCmdCopyImageToBuffer(cmd, images[i], VK_IMAGE_LAYOUT_GENERAL, readback.b,
                                               1, &copy);
                    }
                    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                                         nullptr);
                    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
                    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                    VkFence fence{};
                    CHECK(vkCreateFence(device, &fci, nullptr, &fence) == VK_SUCCESS);
                    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                    si.commandBufferCount = 1;
                    si.pCommandBuffers = &cmd;
                    CHECK(vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS);
                    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 30000000000ull) ==
                          VK_SUCCESS);
                    const size_t bytes = (size_t)width * height * 4;
                    auto a = (uint8_t *)readback.ptr;
                    auto b = a + capacity;
                    CHECK(a[0] == 17 && a[1] == 34 && a[2] == 68 &&
                          a[3] == 255); // Authored LDR void block at physical offset0.
                    for (size_t j = 0; j < bytes; j++) {
                        nonzero += a[j] != 0;
                        if (a[j] != b[j]) {
                            std::fprintf(
                                stderr,
                                "ASTC PIXEL MISMATCH %ux%u native%u clipped%u byte%zu: %u != %u\n",
                                bw, bh, native, clipped, j, a[j], b[j]);
                            return 2;
                        }
                    }
                    for (size_t j = bytes; j < capacity; j++)
                        CHECK(a[j] == 0xa5 && b[j] == 0xa5);
                    compared += bytes;
                    ++cases;
                    CHECK(nonzero > 0);
                    vkDestroyFence(device, fence, nullptr);
                    vkFreeCommandBuffers(device, pool, 1, &cmd);
                    for (unsigned i = 0; i < 2; i++) {
                        vkDestroyImageView(device, views[i], nullptr);
                        vkDestroyImage(device, images[i], nullptr);
                        vkFreeMemory(device, allocations[i], nullptr);
                    }
                }
    CHECK(validation_errors.load() == 0);
    std::printf("PASS full ASTC RGBA8 GPU equality: %u cases,%llu bytes,%llu nonzero bytes;22 "
                "footprints/orientations,two tilings,clipped/full edges\n",
                cases, (unsigned long long)compared, (unsigned long long)nonzero);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    for (auto p : pipelines)
        vkDestroyPipeline(device, p, nullptr);
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorSetLayout(device, dl, nullptr);
    drop(device, input);
    drop(device, readback);
    vkDestroyDevice(device, nullptr);
    if (messenger)
        destroy_debug(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);
#ifdef _WIN32
    FreeLibrary(loader);
#else
    dlclose(loader);
#endif
    return 0;
}
