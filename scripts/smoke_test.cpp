// Runs one compute shader on one named device and checks the answer.
//
// verify.ps1 asks whether the right files are in the right places, which is
// necessary and proves nothing about whether they work together. This does the
// whole thing end to end: GLSL compiled to SPIR-V by the SDK's glslc, a host
// program linked against the SDK's vulkan-1.lib, and arithmetic performed on
// the GPU whose result is checked rather than assumed.
//
// Two things here are not boilerplate, and are why this file is longer than
// the HIP equivalent in the ROCm repository:
//
//   - The validation layer is switched on and its callback sets a flag. A
//     Vulkan program with a misuse in it usually still runs and still prints
//     the right numbers; the only way that failure becomes visible is to make
//     an error from the layer fail the run. Exit code 5 is that.
//
//   - The device is chosen by name. This machine has two Radeons, a discrete
//     7600 XT and the integrated part in the processor, and "device 0" is the
//     integrated one on plenty of machines. A benchmark that quietly ran on
//     the wrong device would look like a slow card.

#include <vulkan/vulkan.h>

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
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "validation: %s\n", data->pMessage);
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) g_validationError = true;
    }
    return VK_FALSE;
}

#define VK_CHECK(expr)                                                            \
    do {                                                                          \
        VkResult vk_result_ = (expr);                                             \
        if (vk_result_ != VK_SUCCESS) {                                           \
            std::fprintf(stderr, "%s -> VkResult %d\n", #expr, (int) vk_result_); \
            return 3;                                                             \
        }                                                                         \
    } while (0)

static const uint32_t     kElements = 1024;
static const VkDeviceSize kBytes    = kElements * sizeof(float);

int main(int argc, char** argv)
{
    const char* spvPath = (argc > 1) ? argv[1] : "smoke_test.spv";
    const std::string wanted = (argc > 2) ? argv[2] : "Radeon RX 7600 XT";

    std::vector<char> code;
    {
        FILE* f = std::fopen(spvPath, "rb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", spvPath); return 2; }
        std::fseek(f, 0, SEEK_END);
        code.resize((size_t) std::ftell(f));
        std::fseek(f, 0, SEEK_SET);
        size_t read = std::fread(code.data(), 1, code.size(), f);
        std::fclose(f);
        // A truncated or empty .spv is accepted by vkCreateShaderModule on
        // some drivers, and then produces a pipeline that does nothing.
        if (read != code.size() || code.size() < 20 || code.size() % 4 != 0) {
            std::fprintf(stderr, "%s is not a whole SPIR-V module (%zu bytes)\n", spvPath, code.size());
            return 2;
        }
    }

    // ---- instance, with validation on -------------------------------------
    const char* layers[]     = { "VK_LAYER_KHRONOS_validation" };
    const char* extensions[] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };

    {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> have(count);
        vkEnumerateInstanceLayerProperties(&count, have.data());
        bool found = false;
        for (const auto& l : have) if (!std::strcmp(l.layerName, layers[0])) found = true;
        if (!found) {
            std::fprintf(stderr, "%s is not enumerable - is VK_ADD_LAYER_PATH set?\n", layers[0]);
            return 2;
        }
    }

    VkDebugUtilsMessengerCreateInfoEXT dbg{};
    dbg.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    dbg.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vulkan-sdk-smoke-test";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    // Chained so that misuse during instance creation itself is reported: the
    // messenger object below cannot exist yet at that point.
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

    // ---- the named device -------------------------------------------------
    uint32_t deviceCount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr));
    if (deviceCount == 0) { std::fprintf(stderr, "no Vulkan devices\n"); return 2; }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data()));

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    for (auto d : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (std::string(p.deviceName).find(wanted) != std::string::npos) { physical = d; props = p; break; }
    }
    if (!physical) {
        std::fprintf(stderr, "no device matching \"%s\". Present:\n", wanted.c_str());
        for (auto d : devices) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(d, &p);
            std::fprintf(stderr, "  %s\n", p.deviceName);
        }
        return 2;
    }

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());

    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    }
    if (family == UINT32_MAX) { std::fprintf(stderr, "no compute queue family\n"); return 2; }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &device));

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    // ---- three host-visible buffers ---------------------------------------
    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memProps);

    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    VkBuffer       buffers[3] = {};
    VkDeviceMemory memory[3]  = {};
    void*          mapped[3]  = {};

    for (int i = 0; i < 3; ++i) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = kBytes;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &buffers[i]));

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, buffers[i], &req);

        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < memProps.memoryTypeCount; ++t) {
            // Both conditions, in this order: the requirements bitmask says
            // which types this buffer may use at all, and only then does
            // HOST_VISIBLE mean anything.
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
        VK_CHECK(vkMapMemory(device, memory[i], 0, kBytes, 0, &mapped[i]));
    }

    float* a = (float*) mapped[0];
    float* b = (float*) mapped[1];
    float* c = (float*) mapped[2];
    // c is filled with a value the shader never writes, so "the shader did
    // nothing" and "the shader wrote zeroes" are different failures.
    for (uint32_t i = 0; i < kElements; ++i) { a[i] = 1.0f; b[i] = 2.0f; c[i] = -1.0f; }

    // ---- descriptors ------------------------------------------------------
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

    VkDescriptorBufferInfo bufferInfo[3] = {};
    VkWriteDescriptorSet   writes[3]     = {};
    for (uint32_t i = 0; i < 3; ++i) {
        bufferInfo[i].buffer = buffers[i];
        bufferInfo[i].range = kBytes;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfo[i];
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

    // ---- pipeline ---------------------------------------------------------
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = code.size();
    smci.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(device, &smci, nullptr, &module));

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VK_CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout));

    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = module;
    cpci.stage.pName = "main";
    cpci.layout = pipelineLayout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VK_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));

    // ---- record and run ---------------------------------------------------
    VkCommandPoolCreateInfo cmdPoolInfo{};
    cmdPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cmdPoolInfo.queueFamilyIndex = family;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &commandPool));

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = commandPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));

    VkCommandBufferBeginInfo cbbi{};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &cbbi));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, kElements / 256, 1, 1);
    VK_CHECK(vkEndCommandBuffer(cmd));

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));

    // A timeout rather than UINT64_MAX: a hung dispatch should end the test,
    // not the shell it was run from.
    VkResult waited = vkWaitForFences(device, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000);
    if (waited == VK_TIMEOUT) { std::fprintf(stderr, "dispatch did not finish in 10s\n"); return 4; }
    VK_CHECK(waited);

    // ---- check ------------------------------------------------------------
    int wrong = 0;
    for (uint32_t i = 0; i < kElements; ++i) {
        if (c[i] != 3.0f) {
            if (wrong < 4) std::fprintf(stderr, "wrong at %u: %f\n", i, c[i]);
            ++wrong;
        }
    }

    // ---- teardown, which is itself a check --------------------------------
    // Every object is destroyed, in order, because the validation layer
    // reports the ones that are not - so a leak here would mean the flag
    // below is reporting on an incomplete run.
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyShaderModule(device, module, nullptr);
    vkDestroyDescriptorPool(device, pool, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    for (int i = 0; i < 3; ++i) {
        vkUnmapMemory(device, memory[i]);
        vkDestroyBuffer(device, buffers[i], nullptr);
        vkFreeMemory(device, memory[i], nullptr);
    }
    vkDestroyDevice(device, nullptr);
    if (messenger && destroyMessenger) destroyMessenger(instance, messenger, nullptr);
    vkDestroyInstance(instance, nullptr);

    if (wrong) { std::fprintf(stderr, "%d of %u elements wrong\n", wrong, kElements); return 4; }
    if (g_validationError) { std::fprintf(stderr, "the answer is right and the validation layer objected\n"); return 5; }

    std::printf("%s: %u elements, all correct, validation clean\n", props.deviceName, kElements);
    return 0;
}
