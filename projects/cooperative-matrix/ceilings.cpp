// Three instruction ceilings on one card, with no memory traffic in any of
// them: fp32 FMA, packed fp16 FMA, and the cooperative matrix multiply-add.
//
// The ROCm repository measured the same three on the same silicon through HIP
// and got 9,513 / 18,633 / 39,077 GFLOP/s. This asks whether Vulkan reaches
// the same units, and how close it gets.
//
// Method, copied deliberately from that branch so the numbers are comparable:
// operands are built once from a runtime value and never reloaded, several
// independent accumulators keep the pipeline fed, each accumulator starts from
// a different value so the compiler cannot fold them into one, and one float
// per thread is written at the end so nothing can be optimised away.
//
// Timing is GPU-side, from a timestamp query pool, not the wall clock. The
// dispatch is the only thing between the two timestamps.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool g_validationError = false;

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        std::fprintf(stderr, "validation: %s\n", data->pMessage);
        g_validationError = true;
    }
    return VK_FALSE;
}

#define VK_CHECK(expr)                                                            \
    do {                                                                          \
        VkResult vk_result_ = (expr);                                             \
        if (vk_result_ != VK_SUCCESS) {                                           \
            std::fprintf(stderr, "%s -> VkResult %d\n", #expr, (int) vk_result_); \
            std::exit(3);                                                         \
        }                                                                         \
    } while (0)

static std::vector<char> readSpv(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(2); }
    std::fseek(f, 0, SEEK_END);
    std::vector<char> code((size_t) std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    size_t read = std::fread(code.data(), 1, code.size(), f);
    std::fclose(f);
    if (read != code.size() || code.size() % 4 != 0) {
        std::fprintf(stderr, "%s is not a whole SPIR-V module\n", path.c_str());
        std::exit(2);
    }
    return code;
}

// One workgroup of 256 across 512 workgroups is 131,072 threads, which is
// about 64 per lane of the card. Enough that scheduling is not the thing being
// measured, small enough that a long iteration count still finishes.
static const uint32_t kLocalSize  = 256;
static const uint32_t kWorkgroups = 512;
static const uint32_t kThreads    = kLocalSize * kWorkgroups;
static const int      kPasses     = 5;

struct Kernel {
    const char* name;
    const char* spv;
    uint32_t    requiredSubgroupSize;   // 0 = let the driver choose
    // Flops for one thread (or, for the matrix kernel, one subgroup) per
    // iteration of the loop.
    double      flopsPerUnitPerIter;
    bool        perSubgroup;
};

int main(int argc, char** argv)
{
    const std::string dir     = (argc > 1) ? argv[1] : ".";
    const std::string wanted  = (argc > 2) ? argv[2] : "Radeon RX 7600 XT";

    // ---- instance ---------------------------------------------------------
    VkDebugUtilsMessengerCreateInfoEXT dbg{};
    dbg.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "coopmat-ceilings";
    // 1.3 because GL_KHR_cooperative_matrix needs SPIR-V 1.6, and 1.6 modules
    // need a 1.3 instance to be accepted.
    app.apiVersion = VK_API_VERSION_1_3;

    const char* layers[]     = { "VK_LAYER_KHRONOS_validation" };
    const char* extensions[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pNext = &dbg;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = extensions;

    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    auto createMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    auto destroyMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (createMessenger) VK_CHECK(createMessenger(instance, &dbg, nullptr, &messenger));

    // ---- device -----------------------------------------------------------
    uint32_t deviceCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr));
    std::vector<VkPhysicalDevice> devices(deviceCount);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()));

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    for (auto d : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (std::string(p.deviceName).find(wanted) != std::string::npos) { physical = d; props = p; break; }
    }
    if (!physical) { std::fprintf(stderr, "no device matching \"%s\"\n", wanted.c_str()); return 2; }

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    }
    if (family == UINT32_MAX) { std::fprintf(stderr, "no compute queue family\n"); return 2; }
    if (families[family].timestampValidBits == 0) {
        std::fprintf(stderr, "queue family %u has no usable timestamps\n", family);
        return 2;
    }

    // Every feature below is needed by one of the three shaders, and a missing
    // one shows up as a pipeline that fails to create with a message about a
    // capability rather than about a feature.
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coopFeatures{};
    coopFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    coopFeatures.cooperativeMatrix = VK_TRUE;

    VkPhysicalDeviceVulkanMemoryModelFeatures memoryModel{};
    memoryModel.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
    memoryModel.vulkanMemoryModel = VK_TRUE;
    memoryModel.pNext = &coopFeatures;

    VkPhysicalDeviceShaderFloat16Int8Features float16{};
    float16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    float16.shaderFloat16 = VK_TRUE;
    float16.pNext = &memoryModel;

    VkPhysicalDeviceSubgroupSizeControlFeatures sizeControl{};
    sizeControl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
    sizeControl.subgroupSizeControl = VK_TRUE;
    sizeControl.computeFullSubgroups = VK_TRUE;
    sizeControl.pNext = &float16;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &sizeControl;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* deviceExtensions[] = { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME };

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &features2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = deviceExtensions;

    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &device));

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    // ---- buffers ----------------------------------------------------------
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    const VkDeviceSize seedBytes   = 256 * sizeof(float);
    const VkDeviceSize resultBytes = (VkDeviceSize) kThreads * sizeof(float);

    VkBuffer       buffers[2] = {};
    VkDeviceMemory memory[2]  = {};
    void*          mapped[2]  = {};
    const VkDeviceSize sizes[2] = { seedBytes, resultBytes };

    for (int i = 0; i < 2; ++i) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = sizes[i];
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &buffers[i]));

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buffers[i], &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < memProps.memoryTypeCount; ++t) {
            if ((req.memoryTypeBits & (1u << t)) &&
                (memProps.memoryTypes[t].propertyFlags & want) == want) { type = t; break; }
        }
        if (type == UINT32_MAX) { std::fprintf(stderr, "no host-visible coherent memory type\n"); return 2; }

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &memory[i]));
        VK_CHECK(vkBindBufferMemory(device, buffers[i], memory[i], 0));
        VK_CHECK(vkMapMemory(device, memory[i], 0, sizes[i], 0, &mapped[i]));
    }

    float* seed = (float*) mapped[0];
    for (int i = 0; i < 256; ++i) seed[i] = 1.0f + 0.001f * (float) i;

    // ---- descriptors, shared by all three pipelines ------------------------
    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 2;
    dslci.pBindings = bindings;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &setLayout));

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &pool));

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &setLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateDescriptorSets(device, &dsai, &set));

    VkDescriptorBufferInfo bufferInfo[2] = {};
    VkWriteDescriptorSet   writes[2]     = {};
    for (uint32_t i = 0; i < 2; ++i) {
        bufferInfo[i].buffer = buffers[i];
        bufferInfo[i].range = sizes[i];
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfo[i];
    }
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    VkPushConstantRange pushRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) };
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pushRange;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout));

    // ---- command buffer and timestamps ------------------------------------
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = family;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &cpci, nullptr, &commandPool));

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = commandPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));

    VkQueryPoolCreateInfo qpci{};
    qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateQueryPool(device, &qpci, nullptr, &queryPool));

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));

    // timestampPeriod is nanoseconds per tick and is not 1 on this hardware.
    const double nsPerTick = props.limits.timestampPeriod;

    auto runOnce = [&](VkPipeline pipeline, uint32_t iters) -> double {
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        vkCmdResetQueryPool(cmd, queryPool, 0, 2);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t), &iters);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
        vkCmdDispatch(cmd, kWorkgroups, 1, 1);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));

        VK_CHECK(vkResetFences(device, 1, &fence));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));

        VkResult waited = vkWaitForFences(device, 1, &fence, VK_TRUE, 30ull * 1000 * 1000 * 1000);
        if (waited == VK_TIMEOUT) { std::fprintf(stderr, "dispatch did not finish in 30s\n"); std::exit(4); }
        VK_CHECK(waited);

        uint64_t stamps[2] = {};
        VK_CHECK(vkGetQueryPoolResults(device, queryPool, 0, 2, sizeof stamps, stamps,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        return (double) (stamps[1] - stamps[0]) * nsPerTick * 1e-9;
    };

    // ---- the three kernels -------------------------------------------------
    // The vector kernels take whatever subgroup width the driver picks; the
    // matrix kernel is run at both widths RDNA3 offers, because the shape list
    // says subgroup scope and says nothing about which width the instruction
    // wants.
    const Kernel kernels[] = {
        { "fp32 vector FMA",          "vec_fp32.spv", 0,  8.0 * 2.0,        false },
        { "fp16 packed FMA",          "vec_fp16.spv", 0,  8.0 * 2.0 * 2.0,  false },
        { "coopmat 16x16x16, wave32", "coopmat.spv",  32, 4.0 * 2.0 * 16 * 16 * 16, true },
        { "coopmat 16x16x16, wave64", "coopmat.spv",  64, 4.0 * 2.0 * 16 * 16 * 16, true },
    };

    std::printf("%s, driver %u.%u.%u\n", props.deviceName,
                VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion),
                VK_VERSION_PATCH(props.driverVersion));
    std::printf("%u threads (%u workgroups of %u), timestampPeriod %.1f ns\n\n",
                kThreads, kWorkgroups, kLocalSize, nsPerTick);
    std::printf("  %-28s %10s %12s %14s\n", "kernel", "iters", "ms", "GFLOP/s");

    double best[4] = {};

    for (int k = 0; k < 4; ++k) {
        const Kernel& kern = kernels[k];
        std::vector<char> code = readSpv(dir + "\\" + kern.spv);

        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = code.size();
        smci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule module = VK_NULL_HANDLE;
        VK_CHECK(vkCreateShaderModule(device, &smci, nullptr, &module));

        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required{};
        required.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
        required.requiredSubgroupSize = kern.requiredSubgroupSize;

        VkComputePipelineCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        if (kern.requiredSubgroupSize) {
            cpi.stage.pNext = &required;
            cpi.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
        }
        cpi.layout = pipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        VkResult made = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline);
        if (made != VK_SUCCESS) {
            std::printf("  %-28s %10s %12s %14s\n", kern.name, "-", "-", "pipeline refused");
            vkDestroyShaderModule(device, module, nullptr);
            continue;
        }

        const uint32_t units = kern.perSubgroup
            ? kThreads / (kern.requiredSubgroupSize ? kern.requiredSubgroupSize : 64)
            : kThreads;

        // A sweep rather than one number: too few iterations and the dispatch
        // is dominated by launch and by the final store, too many and it
        // simply takes longer to say the same thing.
        for (uint32_t iters : { 256u, 1024u, 4096u, 16384u, 65536u }) {
            // Warm up until the card is at speed, not just until the shader is
            // compiled. The first version of this file ran one short dispatch
            // and then measured; the fp32 kernel reported 7,474 GFLOP/s at
            // 4,096 iterations and 14,089 at 16,384, which is not a property
            // of the kernel - a 1 ms dispatch on an idle card spends most of
            // itself below boost clock. Half a second of back-to-back
            // submissions first, and the two agree.
            auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            do { runOnce(pipeline, iters < 4096u ? 4096u : iters); }
            while (std::chrono::steady_clock::now() < until);

            double seconds = 1e30;
            for (int p = 0; p < kPasses; ++p) seconds = std::min(seconds, runOnce(pipeline, iters));

            double flops = (double) units * (double) iters * kern.flopsPerUnitPerIter;
            double gflops = flops / seconds * 1e-9;
            best[k] = std::max(best[k], gflops);

            std::printf("  %-28s %10u %12.3f %14.0f\n",
                        (iters == 256u) ? kern.name : "", iters, seconds * 1e3, gflops);
        }
        std::printf("\n");

        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyShaderModule(device, module, nullptr);
    }

    // ---- what it all means -------------------------------------------------
    double vector32 = best[0];
    std::printf("best of each, and the ratio that is the point of this branch:\n\n");
    for (int k = 0; k < 4; ++k) {
        if (best[k] <= 0.0) continue;
        std::printf("  %-28s %10.0f GFLOP/s   %5.2fx fp32 vector\n",
                    kernels[k].name, best[k], vector32 > 0 ? best[k] / vector32 : 0.0);
    }

    // ---- teardown ----------------------------------------------------------
    vkDestroyFence(device, fence, nullptr);
    vkDestroyQueryPool(device, queryPool, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    for (int i = 0; i < 2; ++i) {
        vkUnmapMemory(device, memory[i]);
        vkDestroyBuffer(device, buffers[i], nullptr);
        vkFreeMemory(device, memory[i], nullptr);
    }
    vkDestroyDevice(device, nullptr);
    if (messenger && destroyMessenger) destroyMessenger(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);

    if (g_validationError) {
        std::fprintf(stderr, "\nthe validation layer objected - the numbers above are not trustworthy\n");
        return 5;
    }
    return 0;
}
