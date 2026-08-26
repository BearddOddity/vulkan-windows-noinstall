// What shapes does this driver actually offer?
//
// VK_KHR_cooperative_matrix does not define a fixed tile size. Each
// implementation publishes a list of (M, N, K, A type, B type, C type, result
// type, scope, saturating) combinations, and a shader may only instantiate one
// that is on the list. Nothing else about the extension can be written until
// that list is known, so this runs first and prints it.
//
// It also prints the two feature bits and the supported stages, because a
// device can advertise the extension and then refuse the feature.

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define VK_CHECK(expr)                                                            \
    do {                                                                          \
        VkResult vk_result_ = (expr);                                             \
        if (vk_result_ != VK_SUCCESS) {                                           \
            std::fprintf(stderr, "%s -> VkResult %d\n", #expr, (int) vk_result_); \
            return 3;                                                             \
        }                                                                         \
    } while (0)

static const char* componentType(VkComponentTypeKHR t)
{
    switch (t) {
        case VK_COMPONENT_TYPE_FLOAT16_KHR: return "fp16";
        case VK_COMPONENT_TYPE_FLOAT32_KHR: return "fp32";
        case VK_COMPONENT_TYPE_FLOAT64_KHR: return "fp64";
        case VK_COMPONENT_TYPE_SINT8_KHR:   return "sint8";
        case VK_COMPONENT_TYPE_SINT16_KHR:  return "sint16";
        case VK_COMPONENT_TYPE_SINT32_KHR:  return "sint32";
        case VK_COMPONENT_TYPE_SINT64_KHR:  return "sint64";
        case VK_COMPONENT_TYPE_UINT8_KHR:   return "uint8";
        case VK_COMPONENT_TYPE_UINT16_KHR:  return "uint16";
        case VK_COMPONENT_TYPE_UINT32_KHR:  return "uint32";
        case VK_COMPONENT_TYPE_UINT64_KHR:  return "uint64";
        default:                            return "?";
    }
}

static const char* scopeName(VkScopeKHR s)
{
    switch (s) {
        case VK_SCOPE_DEVICE_KHR:      return "device";
        case VK_SCOPE_WORKGROUP_KHR:   return "workgroup";
        case VK_SCOPE_SUBGROUP_KHR:    return "subgroup";
        case VK_SCOPE_QUEUE_FAMILY_KHR:return "queue family";
        default:                       return "?";
    }
}

int main(int argc, char** argv)
{
    const std::string wanted = (argc > 1) ? argv[1] : "Radeon RX 7600 XT";

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "coopmat-probe";
    // 1.1 for the physical device properties2 entry points that the
    // cooperative matrix query is built on.
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    for (auto d : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (std::string(p.deviceName).find(wanted) != std::string::npos) { physical = d; props = p; break; }
    }
    if (!physical) { std::fprintf(stderr, "no device matching \"%s\"\n", wanted.c_str()); return 2; }

    std::printf("%s, driver %u.%u.%u, API %u.%u.%u\n\n", props.deviceName,
                VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion),
                VK_VERSION_PATCH(props.driverVersion),
                VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
                VK_VERSION_PATCH(props.apiVersion));

    // The extension has to be in the device's list before any of its entry
    // points may be called, whatever vkGetInstanceProcAddr returns.
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, exts.data());
    bool have = false;
    for (const auto& e : exts) if (!std::strcmp(e.extensionName, "VK_KHR_cooperative_matrix")) have = true;
    std::printf("VK_KHR_cooperative_matrix  %s  (of %u device extensions)\n",
                have ? "present" : "ABSENT", extCount);
    if (!have) return 2;

    VkPhysicalDeviceCooperativeMatrixFeaturesKHR features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR;
    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &features;
    vkGetPhysicalDeviceFeatures2(physical, &features2);

    VkPhysicalDeviceCooperativeMatrixPropertiesKHR cmProps{};
    cmProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &cmProps;
    vkGetPhysicalDeviceProperties2(physical, &props2);

    std::printf("cooperativeMatrix                   = %s\n", features.cooperativeMatrix ? "true" : "false");
    std::printf("cooperativeMatrixRobustBufferAccess = %s\n", features.cooperativeMatrixRobustBufferAccess ? "true" : "false");
    std::printf("supported stages                    = 0x%08x%s\n\n",
                cmProps.cooperativeMatrixSupportedStages,
                (cmProps.cooperativeMatrixSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ? " (compute)" : "");

    // Subgroup size decides how the tile is spread across lanes, and RDNA3 can
    // run either width, so it is printed beside the shapes rather than assumed.
    VkPhysicalDeviceSubgroupSizeControlProperties sizeControl{};
    sizeControl.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
    VkPhysicalDeviceSubgroupProperties subgroup{};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    subgroup.pNext = &sizeControl;
    VkPhysicalDeviceProperties2 props2b{};
    props2b.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2b.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(physical, &props2b);
    std::printf("subgroupSize %u, min %u, max %u\n\n",
                subgroup.subgroupSize, sizeControl.minSubgroupSize, sizeControl.maxSubgroupSize);

    auto getProperties = (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    if (!getProperties) { std::fprintf(stderr, "no vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR\n"); return 2; }

    uint32_t shapeCount = 0;
    VK_CHECK(getProperties(physical, &shapeCount, nullptr));
    std::vector<VkCooperativeMatrixPropertiesKHR> shapes(shapeCount);
    for (auto& s : shapes) s.sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
    VK_CHECK(getProperties(physical, &shapeCount, shapes.data()));

    std::printf("%u supported shapes\n\n", shapeCount);
    std::printf("  %-12s %-7s %-7s %-7s %-7s %-10s %s\n",
                "MxNxK", "A", "B", "C", "result", "scope", "saturating");
    for (const auto& s : shapes) {
        char shape[32];
        std::snprintf(shape, sizeof shape, "%ux%ux%u", s.MSize, s.NSize, s.KSize);
        std::printf("  %-12s %-7s %-7s %-7s %-7s %-10s %s\n",
                    shape,
                    componentType(s.AType), componentType(s.BType),
                    componentType(s.CType), componentType(s.ResultType),
                    scopeName(s.scope),
                    s.saturatingAccumulation ? "yes" : "no");
    }

    vkDestroyInstance(instance, nullptr);
    return 0;
}
