#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include "renderdoc_app.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMMANDS(X) \
    X(vkCreateInstance) X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkDestroyDevice) \
    X(vkGetDeviceQueue) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkBindBufferMemory) \
    X(vkGetBufferMemoryRequirements) X(vkGetBufferDeviceAddress) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdDispatch) \
    X(vkQueueSubmit) X(vkQueueWaitIdle)

#define DECLARE(name) static PFN_##name name;
COMMANDS(DECLARE)
#undef DECLARE

static void die(const char *reason) {
    fprintf(stderr, "%s\n", reason);
    exit(EXIT_FAILURE);
}

#define VK(call) do { \
    VkResult status = (call); \
    if (status != VK_SUCCESS) { \
        fprintf(stderr, "%s returned VkResult %d\n", #call, status); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static void load_vulkan(void) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) die(dlerror());
#define LOAD(name) name = (PFN_##name)dlsym(lib, #name); if (!name) die("Missing " #name);
    COMMANDS(LOAD)
#undef LOAD
}

static RENDERDOC_API_1_6_0 *renderdoc(void) {
    pRENDERDOC_GetAPI get_api = (pRENDERDOC_GetAPI)dlsym(RTLD_DEFAULT, "RENDERDOC_GetAPI");
    if (!get_api) {
        void *loaded = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
        if (loaded) get_api = (pRENDERDOC_GetAPI)dlsym(loaded, "RENDERDOC_GetAPI");
    }
    void *api = NULL;
    if (!get_api || !get_api(eRENDERDOC_API_Version_1_6_0, &api))
        die("RenderDoc API unavailable. Enable its Vulkan layer and preload librenderdoc.so.");
    return api;
}

typedef struct {
    VkBuffer handle;
    VkDeviceMemory memory;
    uint32_t *data;
} HostBuffer;

static VkDeviceMemory allocate(
    VkDevice device, const VkPhysicalDeviceMemoryProperties *memory,
    VkMemoryRequirements requirements, VkMemoryPropertyFlags properties, bool address
) {
    for (uint32_t i = 0; i < memory->memoryTypeCount; ++i) {
        if (!(requirements.memoryTypeBits & (1u << i)) ||
            (memory->memoryTypes[i].propertyFlags & properties) != properties) continue;
        VkMemoryAllocateFlagsInfo flags = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };
        VkMemoryAllocateInfo info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = address ? &flags : NULL,
            .allocationSize = requirements.size,
            .memoryTypeIndex = i,
        };
        VkDeviceMemory result;
        VK(vkAllocateMemory(device, &info, NULL, &result));
        return result;
    }
    die("No compatible Vulkan memory type.");
    return VK_NULL_HANDLE;
}

static HostBuffer host_buffer(
    VkDevice device, const VkPhysicalDeviceMemoryProperties *memory, bool address
) {
    HostBuffer buffer = {0};
    VkBufferCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(uint32_t),
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 (address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0),
    };
    VK(vkCreateBuffer(device, &info, NULL, &buffer.handle));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer.handle, &req);
    buffer.memory = allocate(device, memory, req,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, address);
    VK(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0));
    void *mapped = NULL;
    VK(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    buffer.data = mapped;
    return buffer;
}

static VkShaderModule load_shader(VkDevice device, const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) die("Could not open compute shader.");
    if (fseek(file, 0, SEEK_END) != 0) die("Could not seek shader.");
    long size = ftell(file);
    if (size < 20 || size % 4 || fseek(file, 0, SEEK_SET) != 0) die("Invalid SPIR-V file size.");
    uint32_t *code = malloc((size_t)size);
    if (!code || fread(code, 1, (size_t)size, file) != (size_t)size ||
        code[0] != 0x07230203u) die("Invalid SPIR-V file.");
    fclose(file);
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)size,
        .pCode = code,
    };
    VkShaderModule shader;
    VK(vkCreateShaderModule(device, &info, NULL, &shader));
    free(code);
    return shader;
}

int main(int argc, char **argv) {
    if (argc < 4 || argc > 5 ||
        (strcmp(argv[3], "0") && strcmp(argv[3], "1")) ||
        (argc == 5 && strcmp(argv[4], "--nonuniform")))
        die("Usage: vk-control <shader.spv> <capture-path-template|-> <0|1> [--nonuniform]");
    const bool capture = strcmp(argv[2], "-") != 0;
    const bool nonuniform = argc == 5;
    const uint32_t slot = (uint32_t)(argv[3][0] - '0');
    load_vulkan();

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Headless Vulkan descriptor control",
        .apiVersion = VK_API_VERSION_1_2,
    };
    VkInstanceCreateInfo instance_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance;
    VK(vkCreateInstance(&instance_info, NULL, &instance));
    RENDERDOC_API_1_6_0 *rd = capture ? renderdoc() : NULL;
    if (rd) rd->SetCaptureFilePathTemplate(argv[2]);
    VkPhysicalDevice physical;
    uint32_t device_count = 0;
    VK(vkEnumeratePhysicalDevices(instance, &device_count, NULL));
    if (device_count == 0) die("No Vulkan physical device.");
    VkPhysicalDevice *devices = calloc(device_count, sizeof(*devices));
    if (!devices) die("Could not allocate physical device list.");
    VK(vkEnumeratePhysicalDevices(instance, &device_count, devices));
    physical = devices[0];
    free(devices);

    VkPhysicalDeviceVulkan12Features available12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceFeatures2 available = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &available12,
    };
    vkGetPhysicalDeviceFeatures2(physical, &available);
    if (!available12.bufferDeviceAddress ||
        (!nonuniform && !available.features.shaderStorageImageArrayDynamicIndexing) ||
        (nonuniform && !available12.shaderStorageImageArrayNonUniformIndexing))
        die("Vulkan device lacks buffer device address or requested storage-image indexing.");
    VkPhysicalDeviceVulkan12Features enabled12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .bufferDeviceAddress = VK_TRUE,
        .descriptorIndexing = nonuniform && available12.descriptorIndexing,
        .shaderStorageImageArrayNonUniformIndexing = nonuniform,
    };
    VkPhysicalDeviceFeatures enabled = {
        .shaderStorageImageArrayDynamicIndexing = available.features.shaderStorageImageArrayDynamicIndexing,
    };
    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    if (family_count == 0) die("No Vulkan queue family.");
    VkQueueFamilyProperties *families = calloc(family_count, sizeof(*families));
    if (!families) die("Could not allocate queue family list.");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families);
    uint32_t family = 0;
    while (family < family_count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    free(families);
    if (family == family_count) die("No Vulkan compute queue.");
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo device_info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabled12, .pEnabledFeatures = &enabled,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
    };
    VkDevice device;
    VK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);

    HostBuffer selection = host_buffer(device, &memory, true);
    HostBuffer result = host_buffer(device, &memory, false);
    *selection.data = slot;
    *result.data = 0;
    VkBufferDeviceAddressInfo address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = selection.handle,
    };
    VkDeviceAddress address = vkGetBufferDeviceAddress(device, &address_info);
    if (!address) die("Vulkan selected a null buffer device address.");

    /* Slot 0 views mip 0 (4 by 4, red). Slot 1 views mip 1 (2 by 2, blue). */
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {4, 4, 1}, .mipLevels = 2, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };
    VkImage image;
    VK(vkCreateImage(device, &image_info, NULL, &image));
    VkMemoryRequirements image_req;
    vkGetImageMemoryRequirements(device, image, &image_req);
    VkDeviceMemory image_memory = allocate(device, &memory, image_req,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
    VK(vkBindImageMemory(device, image, image_memory, 0));
    VkImageView views[2];
    VkImageSubresourceRange ranges[2];
    for (uint32_t i = 0; i < 2; ++i) {
        ranges[i] = (VkImageSubresourceRange){
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = i,
            .levelCount = 1, .layerCount = 1,
        };
        VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = image_info.format,
            .subresourceRange = ranges[i],
        };
        VK(vkCreateImageView(device, &view_info, NULL, &views[i]));
    }

    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo set_layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = bindings,
    };
    VkDescriptorSetLayout set_layout;
    VK(vkCreateDescriptorSetLayout(device, &set_layout_info, NULL, &set_layout));
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo pool_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
        .poolSizeCount = 2, .pPoolSizes = sizes,
    };
    VkDescriptorPool pool;
    VK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool));
    VkDescriptorSetAllocateInfo set_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout,
    };
    VkDescriptorSet set;
    VK(vkAllocateDescriptorSets(device, &set_info, &set));
    VkDescriptorImageInfo view_descriptors[] = {
        {VK_NULL_HANDLE, views[0], VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, views[1], VK_IMAGE_LAYOUT_GENERAL},
    };
    VkDescriptorBufferInfo result_descriptor = {result.handle, 0, sizeof(uint32_t)};
    VkWriteDescriptorSet writes[] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
         .descriptorCount = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = view_descriptors},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .pBufferInfo = &result_descriptor},
    };
    vkUpdateDescriptorSets(device, 2, writes, 0, NULL);

    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(address)};
    VkPipelineLayoutCreateInfo pipeline_layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push,
    };
    VkPipelineLayout pipeline_layout;
    VK(vkCreatePipelineLayout(device, &pipeline_layout_info, NULL, &pipeline_layout));
    VkShaderModule shader = load_shader(device, argv[1]);
    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = pipeline_layout,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"},
    };
    VkPipeline pipeline;
    VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));

    VkCommandPoolCreateInfo cmd_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family,
    };
    VkCommandPool cmd_pool;
    VK(vkCreateCommandPool(device, &cmd_pool_info, NULL, &cmd_pool));
    VkCommandBufferAllocateInfo cmd_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    VK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));

    RENDERDOC_DevicePointer rd_device = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance);
    if (rd) {
        rd->StartFrameCapture(rd_device, NULL);
        if (!rd->IsFrameCapturing()) die("RenderDoc did not start a Vulkan frame capture.");
        rd->SetCaptureTitle("Vulkan compute: BDA selects image mip");
    }
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK(vkBeginCommandBuffer(cmd, &begin));
    VkImageMemoryBarrier image_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 2, .layerCount = 1},
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &image_barrier);
    const VkClearColorValue colors[] = {
        {.float32 = {1.0f, 0.0f, 0.0f, 1.0f}},
        {.float32 = {0.0f, 0.0f, 1.0f, 1.0f}},
    };
    for (uint32_t i = 0; i < 2; ++i)
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colors[i], 1, &ranges[i]);
    image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &image_barrier);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(address), &address);
    vkCmdDispatch(cmd, 1, 1, 1);
    VkMemoryBarrier host_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 1, &host_barrier, 0, NULL, 0, NULL);
    VK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };
    VK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
    VK(vkQueueWaitIdle(queue));
    uint32_t actual = *result.data;
    const uint32_t expected = slot == 0 ? 0xff0000ffu : 0xffff0000u;
    printf("slot=%u mip=%u actual=0x%08x expected=0x%08x\n", slot, slot, actual, expected);
    if (rd) {
        if (!rd->EndFrameCapture(rd_device, NULL) || rd->GetNumCaptures() != 1)
            die("RenderDoc did not produce exactly one frame capture.");
        char path[4096];
        uint32_t path_size = sizeof(path);
        if (rd->GetCapture(0, path, &path_size, NULL)) printf("capture=%s\n", path);
    }

    vkDestroyCommandPool(device, cmd_pool, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyShaderModule(device, shader, NULL);
    vkDestroyPipelineLayout(device, pipeline_layout, NULL);
    vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    for (int i = 0; i < 2; ++i) vkDestroyImageView(device, views[i], NULL);
    vkDestroyImage(device, image, NULL);
    vkFreeMemory(device, image_memory, NULL);
    HostBuffer buffers[] = {selection, result};
    for (int i = 0; i < 2; ++i) {
        vkUnmapMemory(device, buffers[i].memory);
        vkDestroyBuffer(device, buffers[i].handle, NULL);
        vkFreeMemory(device, buffers[i].memory, NULL);
    }
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    return actual == expected ? EXIT_SUCCESS : EXIT_FAILURE;
}
