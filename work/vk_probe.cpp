// Prints the Vulkan capabilities of the first GPU that matter for the renderer design (vk_probe.exe), and checks that DXC works in-process from MinGW.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

int main()
{
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { printf("no vulkan-1.dll\n"); return 1; }
    auto gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    auto createInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    auto enumVer = (PFN_vkEnumerateInstanceVersion)gipa(nullptr, "vkEnumerateInstanceVersion");
    uint32_t iv = 0; if (enumVer) enumVer(&iv);
    printf("instance version %u.%u.%u\n", VK_API_VERSION_MAJOR(iv), VK_API_VERSION_MINOR(iv), VK_API_VERSION_PATCH(iv));
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ci.pApplicationInfo = &ai;
    VkInstance inst; if (createInstance(&ci, nullptr, &inst) != VK_SUCCESS) { printf("vkCreateInstance failed\n"); return 2; }
#define F(n) auto n = (PFN_##n)gipa(inst, #n)
    F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceProperties2); F(vkGetPhysicalDeviceFeatures2); F(vkEnumerateDeviceExtensionProperties); F(vkGetPhysicalDeviceFormatProperties); F(vkGetPhysicalDeviceMemoryProperties);
    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n); vkEnumeratePhysicalDevices(inst, &n, pds.data());
    for (VkPhysicalDevice pd : pds)
    {
        VkPhysicalDevicePushDescriptorPropertiesKHR pdp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 }; p2.pNext = &pdp;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        const VkPhysicalDeviceProperties& p = p2.properties;
        printf("device %s api %u.%u.%u driver %08X type %d\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, p.deviceType);
        const VkPhysicalDeviceLimits& l = p.limits;
        printf("  perStage: samplers %u ubo %u ssbo %u sampled %u | perSet: ubo %u uboDyn %u ssbo %u | boundSets %u pushConst %u maxPushDescriptors %u\n", l.maxPerStageDescriptorSamplers, l.maxPerStageDescriptorUniformBuffers, l.maxPerStageDescriptorStorageBuffers,
               l.maxPerStageDescriptorSampledImages, l.maxDescriptorSetUniformBuffers, l.maxDescriptorSetUniformBuffersDynamic, l.maxDescriptorSetStorageBuffers, l.maxBoundDescriptorSets, l.maxPushConstantsSize, pdp.maxPushDescriptors);
        printf("  uboAlign %llu ssboAlign %llu maxUboRange %u maxSsboRange %u timestampPeriod %g maxSamplerAniso %g nonCoherentAtom %llu\n", (unsigned long long)l.minUniformBufferOffsetAlignment, (unsigned long long)l.minStorageBufferOffsetAlignment, l.maxUniformBufferRange, l.maxStorageBufferRange, l.timestampPeriod, l.maxSamplerAnisotropy, (unsigned long long)l.nonCoherentAtomSize);
        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT eds = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT };
        VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES }; f12.pNext = &eds;
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 }; f2.pNext = &f12;
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        const VkPhysicalDeviceFeatures& f = f2.features;
        printf("  features: geometryShader %d depthClamp %d samplerAniso %d bc %d indepBlend %d dualSrc %d fillNonSolid %d shaderInt16 %d | timelineSem %d descIndexing %d | extDynState %d\n", f.geometryShader, f.depthClamp, f.samplerAnisotropy,
               f.textureCompressionBC, f.independentBlend, f.dualSrcBlend, f.fillModeNonSolid, f.shaderInt16, f12.timelineSemaphore, f12.descriptorIndexing, eds.extendedDynamicState);
        uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ex(ne); vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, ex.data());
        const char* want[] = { "VK_KHR_swapchain", "VK_KHR_push_descriptor", "VK_KHR_dynamic_rendering", "VK_EXT_extended_dynamic_state", "VK_EXT_extended_dynamic_state2", "VK_EXT_depth_clip_enable", "VK_KHR_maintenance1", "VK_EXT_custom_border_color", "VK_KHR_imageless_framebuffer", "VK_KHR_synchronization2", "VK_EXT_descriptor_indexing", "VK_KHR_timeline_semaphore", "VK_EXT_memory_budget", "VK_KHR_pipeline_library", "VK_EXT_graphics_pipeline_library", "VK_EXT_shader_demote_to_helper_invocation" };
        for (const char* w : want)
        {
            bool have = false; for (auto& e : ex) if (!strcmp(e.extensionName, w)) have = true;
            printf("  %-44s %s\n", w, have ? "yes" : "-");
        }
        auto fmt = [&](VkFormat vf, const char* name)
        {
            VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, vf, &fp);
            printf("  format %-28s optimal %08X\n", name, fp.optimalTilingFeatures);
        };
        fmt(VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT"); fmt(VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT"); fmt(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1"); fmt(VK_FORMAT_R16G16B16A16_UNORM, "RGBA16_UNORM");
        fmt(VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10"); fmt(VK_FORMAT_R16G16_UNORM, "RG16_UNORM"); fmt(VK_FORMAT_R8G8B8A8_SRGB, "RGBA8_SRGB");
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) printf("  memtype %u flags %X heap %u (%llu MB, flags %X)\n", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex, (unsigned long long)(mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size >> 20), mp.memoryHeaps[mp.memoryTypes[i].heapIndex].flags);
    }
    return 0;
}
