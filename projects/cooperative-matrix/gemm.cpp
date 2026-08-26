// C = A*B on the card, twice: once through cooperative matrix and once the
// obvious scalar way, both fp16 in and fp32 out, both checked against the
// processor before either rate is printed.
//
// The ceilings program answers "how fast is the instruction with no memory
// traffic". This answers the question that follows: how much of that survives
// having to fetch the operands. The ROCm repository's equivalent pair found
// hipBLASLt reaching 80% of the WMMA ceiling where a hand-written blocked
// tiling reached 20%, and the kernel here is closer to the hand-written one -
// there is no shared-memory staging in it at all.
//
// The buffers are DEVICE_LOCAL and are filled through a staging copy. The
// first version used host-visible memory for everything, which is system RAM
// across PCIe on this card, and measured the bus.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

// Minimal fp16, host side. Only needs to encode the small exact values this
// test uses, and to decode whatever comes back.
static uint16_t toHalf(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t  exp  = (int32_t) ((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x7FFFFFu;
    if (exp <= 0)   return (uint16_t) sign;
    if (exp >= 31)  return (uint16_t) (sign | 0x7C00u);
    return (uint16_t) (sign | ((uint32_t) exp << 10) | (mant >> 13));
}

static float fromHalf(uint16_t h)
{
    uint32_t sign = (uint32_t) (h & 0x8000u) << 16;
    uint32_t exp  = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0)       bits = sign;
    else if (exp == 31) bits = sign | 0x7F800000u | (mant << 13);
    else                bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

static std::vector<char> readSpv(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); std::exit(2); }
    std::fseek(f, 0, SEEK_END);
    std::vector<char> code((size_t) std::ftell(f));
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(code.data(), 1, code.size(), f) != code.size()) { std::exit(2); }
    std::fclose(f);
    return code;
}

// 2048 rather than 1024. At 1024 the matrix kernels finish in a tenth of a
// millisecond and the same binary reported 8,516 and 9,475 GFLOP/s on
// consecutive runs - a dispatch that short is measuring its own launch as much
// as its arithmetic. At 2048 the run-to-run spread closes to about 1%.
static const uint32_t M = 2048, N = 2048, K = 2048;
static const int      kPasses = 5;

int main(int argc, char** argv)
{
    const std::string dir    = (argc > 1) ? argv[1] : ".";
    const std::string wanted = (argc > 2) ? argv[2] : "Radeon RX 7600 XT";

    VkDebugUtilsMessengerCreateInfoEXT dbg{};
    dbg.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "coopmat-gemm";
    app.apiVersion = VK_API_VERSION_1_3;

    const char* layers[]     = { "VK_LAYER_KHRONOS_validation" };
    const char* instExts[]   = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pNext = &dbg;
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;
    ici.enabledExtensionCount = 1;
    ici.ppEnabledExtensionNames = instExts;

    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    auto createMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    auto destroyMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (createMessenger) VK_CHECK(createMessenger(instance, &dbg, nullptr, &messenger));

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

    // Both shaders read fp16 out of a storage buffer, which is a feature of
    // its own and not implied by shaderFloat16.
    VkPhysicalDevice16BitStorageFeatures storage16{};
    storage16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    storage16.storageBuffer16BitAccess = VK_TRUE;
    storage16.pNext = &float16;

    VkPhysicalDeviceSubgroupSizeControlFeatures sizeControl{};
    sizeControl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
    sizeControl.subgroupSizeControl = VK_TRUE;
    sizeControl.computeFullSubgroups = VK_TRUE;
    sizeControl.pNext = &storage16;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &sizeControl;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* deviceExts[] = { VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME };

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &features2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = deviceExts;

    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

    auto makeBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags want,
                          VkBuffer& buffer, VkDeviceMemory& mem) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = size;
        bci.usage = usage;
        VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &buffer));
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buffer, &req);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < memProps.memoryTypeCount; ++t) {
            if ((req.memoryTypeBits & (1u << t)) &&
                (memProps.memoryTypes[t].propertyFlags & want) == want) { type = t; break; }
        }
        if (type == UINT32_MAX) { std::fprintf(stderr, "no memory type for 0x%x\n", want); std::exit(2); }
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = type;
        VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &mem));
        VK_CHECK(vkBindBufferMemory(device, buffer, mem, 0));
    };

    const VkDeviceSize bytesA = (VkDeviceSize) M * K * sizeof(uint16_t);
    const VkDeviceSize bytesB = (VkDeviceSize) K * N * sizeof(uint16_t);
    const VkDeviceSize bytesC = (VkDeviceSize) M * N * sizeof(float);
    const VkDeviceSize bytesStaging = std::max(bytesC, std::max(bytesA, bytesB));

    VkBuffer bufA, bufB, bufC, bufStaging;
    VkDeviceMemory memA, memB, memC, memStaging;

    // DEVICE_LOCAL without HOST_VISIBLE is heap 0 on this card: the 15.73 GiB
    // of board memory rather than the 256 MiB window into it.
    makeBuffer(bytesA, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bufA, memA);
    makeBuffer(bytesB, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bufB, memB);
    makeBuffer(bytesC, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, bufC, memC);
    makeBuffer(bytesStaging, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
               bufStaging, memStaging);

    void* staging = nullptr;
    VK_CHECK(vkMapMemory(device, memStaging, 0, bytesStaging, 0, &staging));

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

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));

    auto submitAndWait = [&]() {
        VK_CHECK(vkResetFences(device, 1, &fence));
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        VkResult waited = vkWaitForFences(device, 1, &fence, VK_TRUE, 60ull * 1000 * 1000 * 1000);
        if (waited == VK_TIMEOUT) { std::fprintf(stderr, "dispatch did not finish in 60s\n"); std::exit(4); }
        VK_CHECK(waited);
    };

    auto copyToDevice = [&](VkBuffer dst, VkDeviceSize size) {
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        VkBufferCopy region{ 0, 0, size };
        vkCmdCopyBuffer(cmd, bufStaging, dst, 1, &region);
        VK_CHECK(vkEndCommandBuffer(cmd));
        submitAndWait();
    };

    // ---- operands ----------------------------------------------------------
    // Small values with exact fp16 representations, varying enough that a
    // kernel reading the wrong tile gets a visibly wrong answer. A matrix of
    // ones would multiply correctly however badly the indexing was wrong.
    std::vector<uint16_t> hostA((size_t) M * K), hostB((size_t) K * N);
    for (uint32_t i = 0; i < M; ++i)
        for (uint32_t k = 0; k < K; ++k)
            hostA[(size_t) i * K + k] = toHalf((float) ((i * 3 + k * 7) % 13 - 6) * 0.25f);
    for (uint32_t k = 0; k < K; ++k)
        for (uint32_t j = 0; j < N; ++j)
            hostB[(size_t) k * N + j] = toHalf((float) ((k * 5 + j * 11) % 11 - 5) * 0.25f);

    std::memcpy(staging, hostA.data(), bytesA);
    copyToDevice(bufA, bytesA);
    std::memcpy(staging, hostB.data(), bytesB);
    copyToDevice(bufB, bytesB);

    // ---- descriptors -------------------------------------------------------
    VkDescriptorSetLayoutBinding bindings[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 3;
    dslci.pBindings = bindings;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorSetLayout(device, &dslci, nullptr, &setLayout));

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 };
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

    VkBuffer setBuffers[3] = { bufA, bufB, bufC };
    VkDeviceSize setSizes[3] = { bytesA, bytesB, bytesC };
    VkDescriptorBufferInfo bufferInfo[3] = {};
    VkWriteDescriptorSet   writes[3]     = {};
    for (uint32_t i = 0; i < 3; ++i) {
        bufferInfo[i].buffer = setBuffers[i];
        bufferInfo[i].range = setSizes[i];
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfo[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    struct { uint32_t M, N, K; } push{ M, N, K };
    VkPushConstantRange pushRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push };
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pushRange;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout));

    VkQueryPoolCreateInfo qpci{};
    qpci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateQueryPool(device, &qpci, nullptr, &queryPool));

    const double nsPerTick = props.limits.timestampPeriod;

    auto runDispatch = [&](VkPipeline pipeline, uint32_t gx, uint32_t gy) -> double {
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        vkCmdResetQueryPool(cmd, queryPool, 0, 2);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, &push);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, 0);
        vkCmdDispatch(cmd, gx, gy, 1);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));
        submitAndWait();
        uint64_t stamps[2] = {};
        VK_CHECK(vkGetQueryPoolResults(device, queryPool, 0, 2, sizeof stamps, stamps,
                                       sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        return (double) (stamps[1] - stamps[0]) * nsPerTick * 1e-9;
    };

    auto readBackC = [&](std::vector<float>& out) {
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo cbbi{};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &cbbi));
        VkBufferCopy region{ 0, 0, bytesC };
        vkCmdCopyBuffer(cmd, bufC, bufStaging, 1, &region);
        VK_CHECK(vkEndCommandBuffer(cmd));
        submitAndWait();
        out.resize((size_t) M * N);
        std::memcpy(out.data(), staging, bytesC);
    };

    // The reference. Every element would be 2^30 multiply-adds on the
    // processor, so a sample is taken instead - but a fixed, spread-out sample
    // rather than a random one, so a failure is reproducible.
    auto checkSample = [&](const std::vector<float>& got) -> double {
        double worst = 0.0;
        for (uint32_t s = 0; s < 512; ++s) {
            uint32_t i = (s * 37) % M;
            uint32_t j = (s * 53 + 11) % N;
            double ref = 0.0;
            for (uint32_t k = 0; k < K; ++k)
                ref += (double) fromHalf(hostA[(size_t) i * K + k]) *
                       (double) fromHalf(hostB[(size_t) k * N + j]);
            double mine = got[(size_t) i * N + j];
            double denom = std::max(1.0, std::fabs(ref));
            worst = std::max(worst, std::fabs(mine - ref) / denom);
        }
        return worst;
    };

    struct Job {
        const char* name;
        const char* spv;
        uint32_t    requiredSubgroupSize;
        uint32_t    gx, gy;
    };
    const Job jobs[] = {
        // 128 threads per workgroup at subgroup size 32 is four subgroups,
        // each owning one 16x16 tile of C, so one workgroup covers 64 columns.
        { "cooperative matrix", "gemm_coopmat.spv", 32, N / 64,  M / 16 },
        // Four subgroups, each owning 32 columns, so one workgroup covers 128.
        { "coopmat, 2x2 blocked", "gemm_blocked.spv", 32, N / 128, M / 32 },
        { "scalar",             "gemm_scalar.spv",   0, N / 16,  M / 16 },
    };

    std::printf("%s, driver %u.%u.%u\n", props.deviceName,
                VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion),
                VK_VERSION_PATCH(props.driverVersion));
    std::printf("C = A*B, %ux%ux%u, fp16 in, fp32 accumulate, device-local operands\n", M, N, K);
    std::printf("%.1f MB in, %.1f MB out, %.2f GFLOP of arithmetic\n\n",
                (double) (bytesA + bytesB) / 1e6, (double) bytesC / 1e6,
                2.0 * M * N * K * 1e-9);
    std::printf("  %-22s %10s %12s %14s\n", "kernel", "ms", "GFLOP/s", "worst rel. err");

    for (const Job& job : jobs) {
        std::vector<char> code = readSpv(dir + "\\" + job.spv);
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = code.size();
        smci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule module = VK_NULL_HANDLE;
        VK_CHECK(vkCreateShaderModule(device, &smci, nullptr, &module));

        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo required{};
        required.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
        required.requiredSubgroupSize = job.requiredSubgroupSize;

        VkComputePipelineCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        if (job.requiredSubgroupSize) {
            cpi.stage.pNext = &required;
            cpi.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
        }
        cpi.layout = pipelineLayout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline));

        // Clocks, again. A single GEMM here is a few milliseconds and an idle
        // card spends most of that below boost.
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        do { runDispatch(pipeline, job.gx, job.gy); }
        while (std::chrono::steady_clock::now() < until);

        double seconds = 1e30;
        for (int p = 0; p < kPasses; ++p) seconds = std::min(seconds, runDispatch(pipeline, job.gx, job.gy));

        std::vector<float> got;
        readBackC(got);
        double err = checkSample(got);

        double gflops = 2.0 * M * N * K / seconds * 1e-9;
        std::printf("  %-22s %10.3f %12.0f %14.2e%s\n",
                    job.name, seconds * 1e3, gflops, err,
                    err > 2e-2 ? "   WRONG" : "");

        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyShaderModule(device, module, nullptr);
    }

    vkDestroyQueryPool(device, queryPool, nullptr);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    vkUnmapMemory(device, memStaging);
    for (VkBuffer b : { bufA, bufB, bufC, bufStaging }) vkDestroyBuffer(device, b, nullptr);
    for (VkDeviceMemory m : { memA, memB, memC, memStaging }) vkFreeMemory(device, m, nullptr);
    vkDestroyDevice(device, nullptr);
    if (messenger && destroyMessenger) destroyMessenger(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);

    if (g_validationError) {
        std::fprintf(stderr, "\nthe validation layer objected - the numbers above are not trustworthy\n");
        return 5;
    }
    return 0;
}
