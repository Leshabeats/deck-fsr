// SPDX-License-Identifier: MIT
#include "ffx_vk_portable.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include "vulkan_fixture.h"

namespace {
constexpr FfxVkPortableExtent2D Render{854, 534};
constexpr FfxVkPortableExtent2D Output{1280, 800};
constexpr uint32_t Warmup = 8;
constexpr uint32_t Samples = 40;
constexpr VkDeviceSize OutputBytes = VkDeviceSize{Output.width} * Output.height * 8;

void check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
    }
}

void checkFsr(FfxVkPortableResult result, const char* operation)
{
    if (result != FFX_VK_PORTABLE_OK) {
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
    }
}

// Match the public API smoke's Vulkan 1.1 core fallback for memory requirements.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice device, const char* name)
{
    if (std::strcmp(name, "vkGetBufferMemoryRequirements2KHR") == 0) return nullptr;
    return vkGetDeviceProcAddr(device, name);
}

enum ImageIndex { Color, Depth, Motion, Upscaled, ImageCount };

struct Benchmark {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    uint32_t timestampBits = 0;
    FfxVkPortableUpscaleContext* fsr = nullptr;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    std::array<TestImage, ImageCount> images{};
    ReadbackBuffer readback{};

    ~Benchmark()
    {
        if (device) {
            vkDeviceWaitIdle(device);
            ffxVkPortableUpscaleContextDestroy(fsr);
            if (commands) vkDestroyCommandPool(device, commands, nullptr);
            if (timestamps) vkDestroyQueryPool(device, timestamps, nullptr);
            for (auto& image : images) destroyImage(device, &image);
            destroyReadbackBuffer(device, &readback);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    bool selectDevice()
    {
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerating Vulkan devices");
        std::vector<VkPhysicalDevice> devices(count);
        if (devices.empty()) return false;
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "reading Vulkan devices");
        int bestScore = -1;
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties candidateProperties{};
            vkGetPhysicalDeviceProperties(candidate, &candidateProperties);
            if (candidateProperties.apiVersion < VK_API_VERSION_1_3 ||
                candidateProperties.limits.timestampPeriod <= 0) {
                continue;
            }
            FfxVkPortableDeviceCapabilities caps{};
            caps.structSize = sizeof(caps);
            if (ffxVkPortableQueryDeviceCapabilities(candidate, &caps) != FFX_VK_PORTABLE_OK ||
                !caps.fsr3ComputePrerequisites) {
                continue;
            }
            uint32_t familyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
            for (uint32_t i = 0; i < familyCount; ++i) {
                const auto& family = families[i];
                if (!family.queueCount || !(family.queueFlags & VK_QUEUE_COMPUTE_BIT) ||
                    !family.timestampValidBits) {
                    continue;
                }
                int score = candidateProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 100 : 10;
                if (!(family.queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++score;
                if (score > bestScore) {
                    physical = candidate;
                    properties = candidateProperties;
                    queueFamily = i;
                    timestampBits = family.timestampValidBits;
                    bestScore = score;
                }
            }
        }
        return physical != VK_NULL_HANDLE;
    }

    bool setup()
    {
        uint32_t version = 0;
        check(vkEnumerateInstanceVersion(&version), "reading Vulkan loader version");
        if (version < VK_API_VERSION_1_3) return false;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "FSR 3.1.4 headless benchmark";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        check(vkCreateInstance(&instanceInfo, nullptr, &instance), "creating Vulkan instance");
        if (!selectDevice()) return false;

        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &features12;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        enabled12.shaderFloat16 = features12.shaderFloat16;
        enabled12.shaderStorageBufferArrayNonUniformIndexing =
            features12.shaderStorageBufferArrayNonUniformIndexing;
        VkPhysicalDeviceFeatures enabled{};
        enabled.shaderStorageImageWriteWithoutFormat = features.features.shaderStorageImageWriteWithoutFormat;
        enabled.shaderStorageImageReadWithoutFormat = features.features.shaderStorageImageReadWithoutFormat;
        enabled.shaderStorageImageExtendedFormats = features.features.shaderStorageImageExtendedFormats;
        enabled.shaderInt16 = features.features.shaderInt16;
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.pNext = &enabled12;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.pEnabledFeatures = &enabled;
        check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "creating Vulkan device");
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        FfxVkPortableDeviceInfo portable{};
        portable.structSize = sizeof(portable);
        portable.instance = instance;
        portable.physicalDevice = physical;
        portable.device = device;
        portable.getDeviceProcAddr = deviceProc;
        portable.queue = queue;
        portable.queueFamilyIndex = queueFamily;
        portable.shaderFloat16Enabled = enabled12.shaderFloat16;
        portable.shaderStorageBufferArrayNonUniformIndexingEnabled =
            enabled12.shaderStorageBufferArrayNonUniformIndexing;
        portable.shaderStorageImageWriteWithoutFormatEnabled = enabled.shaderStorageImageWriteWithoutFormat;
        FfxVkPortableUpscaleCreateInfo info{};
        info.structSize = sizeof(info);
        info.flags = FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT | FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
        info.maxRenderSize = Render;
        info.maxOutputSize = Output;
        checkFsr(ffxVkPortableUpscaleContextCreate(&portable, &info, &fsr), "creating FSR 3.1.4 context");

        constexpr VkImageUsageFlags InputUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        constexpr VkImageUsageFlags OutputUsage =
            InputUsage | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!createImage(physical, device, VK_FORMAT_R16G16B16A16_SFLOAT,
                         Render.width, Render.height, InputUsage, &images[Color]) ||
            !createImage(physical, device, VK_FORMAT_R32_SFLOAT,
                         Render.width, Render.height, InputUsage, &images[Depth]) ||
            !createImage(physical, device, VK_FORMAT_R16G16_SFLOAT,
                         Render.width, Render.height, InputUsage, &images[Motion]) ||
            !createImage(physical, device, VK_FORMAT_R16G16B16A16_SFLOAT,
                         Output.width, Output.height, OutputUsage, &images[Upscaled]) ||
            !createReadbackBuffer(physical, device, OutputBytes, &readback)) {
            throw std::runtime_error("allocating test images and readback");
        }
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = queueFamily;
        check(vkCreateCommandPool(device, &pool, nullptr, &commands), "creating command pool");
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = commands;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &allocate, &command), "allocating command buffer");
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query.queryCount = Samples * 2;
        check(vkCreateQueryPool(device, &query, nullptr, &timestamps), "allocating timestamp queries");
        return true;
    }

    void begin()
    {
        check(vkResetCommandPool(device, commands, 0), "resetting command pool");
        VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &info), "beginning command buffer");
    }

    void submit()
    {
        check(vkEndCommandBuffer(command), "ending command buffer");
        VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        info.commandBufferCount = 1;
        info.pCommandBuffers = &command;
        check(vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE), "submitting commands");
        check(vkQueueWaitIdle(queue), "waiting for GPU");
    }

    void imageBarriers(VkPipelineStageFlags source, VkPipelineStageFlags destination,
                       uint32_t count, const VkImageMemoryBarrier* barriers)
    {
        vkCmdPipelineBarrier(command, source, destination, 0, 0, nullptr, 0, nullptr, count, barriers);
    }

    void clearImage(ImageIndex image, const VkClearColorValue& color)
    {
        const VkImageSubresourceRange wholeImage{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, images[image].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &color, 1, &wholeImage);
    }

    void initializeImages()
    {
        begin();
        vkCmdResetQueryPool(command, timestamps, 0, Samples * 2);
        std::array<VkImageMemoryBarrier, ImageCount> barriers{};
        for (uint32_t i = 0; i < ImageCount; ++i) {
            barriers[i] = imageBarrier(images[i].image, VK_IMAGE_LAYOUT_UNDEFINED,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        imageBarriers(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      barriers.size(), barriers.data());
        const VkClearColorValue color{{0.25f, 0.5f, 0.75f, 1.0f}};
        const VkClearColorValue depth{{0.5f, 0.0f, 0.0f, 0.0f}};
        const VkClearColorValue motion{{0.0f, 0.0f, 0.0f, 0.0f}};
        const VkClearColorValue poison{{-1.0f, -1.0f, -1.0f, -1.0f}};
        clearImage(Color, color);
        clearImage(Depth, depth);
        clearImage(Motion, motion);
        clearImage(Upscaled, poison);
        for (uint32_t i = 0; i < ImageCount; ++i) {
            const bool isOutput = i == Upscaled;
            barriers[i] = imageBarrier(images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       isOutput ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                       VK_ACCESS_TRANSFER_WRITE_BIT,
                                       isOutput ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT);
        }
        imageBarriers(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      barriers.size(), barriers.data());
        submit();
    }

    void dispatchFrame(uint64_t frame, int sample = -1, bool verify = false)
    {
        FfxVkPortableUpscaleDispatchInfo dispatch{};
        dispatch.structSize = sizeof(dispatch);
        dispatch.commandBuffer = command;
        dispatch.color = asPortableImage(images[Color], FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
        dispatch.depth = asPortableImage(images[Depth], FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
        dispatch.motionVectors = asPortableImage(images[Motion], FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
        dispatch.exposure.structSize = sizeof(dispatch.exposure);
        dispatch.reactiveMask.structSize = sizeof(dispatch.reactiveMask);
        dispatch.transparencyAndCompositionMask.structSize = sizeof(dispatch.transparencyAndCompositionMask);
        dispatch.output = asPortableImage(images[Upscaled], FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.motionVectorScale = {static_cast<float>(Render.width), static_cast<float>(Render.height)};
        dispatch.renderSize = Render;
        dispatch.outputSize = Output;
        dispatch.frameTimeMilliseconds = 1000.0f / 60.0f;
        dispatch.preExposure = 1.0f;
        dispatch.reset = frame == 1 ? VK_TRUE : VK_FALSE;
        dispatch.cameraNear = 0.1f;
        dispatch.cameraFar = 1000.0f;
        dispatch.cameraVerticalFovRadians = 1.0f;
        dispatch.viewSpaceToMeters = 1.0f;
        dispatch.frameId = frame;

        begin();
        const auto reuse = imageBarrier(images[Upscaled].image, VK_IMAGE_LAYOUT_GENERAL,
                                        VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                                        VK_ACCESS_SHADER_WRITE_BIT);
        if (frame > 1) imageBarriers(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, &reuse);
        // Waiting for the previous submission keeps the context's per-frame
        // resources serial. No upload, clear, or readback is recorded between timestamps.
        if (sample >= 0) {
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, 2 * sample);
        }
        checkFsr(ffxVkPortableUpscaleContextRecordDispatch(fsr, &dispatch), "recording FSR upscale");
        if (sample >= 0) {
            vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, 2 * sample + 1);
        }
        if (verify) recordReadback();
        submit();
    }

    void recordReadback()
    {
        const VkImage source = images[Upscaled].image;
        const auto copyBarrier = imageBarrier(source, VK_IMAGE_LAYOUT_GENERAL,
                                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                              VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        imageBarriers(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, &copyBarrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {Output.width, Output.height, 1};
        vkCmdCopyImageToBuffer(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);

        const auto restoreBarrier = imageBarrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                 VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                                                 VK_ACCESS_SHADER_WRITE_BIT);
        imageBarriers(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, &restoreBarrier);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = readback.buffer;
        host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                             0, nullptr, 1, &host, 0, nullptr);
    }

    void poisonOutput()
    {
        begin();
        const VkImage output = images[Upscaled].image;
        const auto toTransfer = imageBarrier(output, VK_IMAGE_LAYOUT_GENERAL,
                                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        imageBarriers(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 1, &toTransfer);
        const VkClearColorValue poison{{-1.0f, -1.0f, -1.0f, -1.0f}};
        clearImage(Upscaled, poison);
        const auto toCompute = imageBarrier(output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                            VK_ACCESS_SHADER_WRITE_BIT);
        imageBarriers(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1, &toCompute);
        submit();
    }

    void checkReadback(const char* label)
    {
        void* data = nullptr;
        check(vkMapMemory(device, readback.memory, 0, VK_WHOLE_SIZE, 0, &data), "mapping readback");
        if (!(readback.memoryProperties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
            range.memory = readback.memory;
            range.size = VK_WHOLE_SIZE;
            const VkResult result = vkInvalidateMappedMemoryRanges(device, 1, &range);
            if (result != VK_SUCCESS) {
                vkUnmapMemory(device, readback.memory);
                check(result, "invalidating readback");
            }
        }
        const auto* components = static_cast<const uint16_t*>(data);
        std::size_t valid = 0;
        constexpr std::size_t Pixels = std::size_t{Output.width} * Output.height;
        for (std::size_t i = 0; i < Pixels; ++i) {
            const uint16_t* rgba = components + 4 * i;
            // Positive, finite half-floats preserve numerical order in their
            // bit patterns. Alpha is 1.0 and the poison represents -1.0.
            if (rgba[0] > 0 && rgba[0] < rgba[1] && rgba[1] < rgba[2] &&
                rgba[2] < 0x7c00u && rgba[3] == 0x3c00u) {
                ++valid;
            }
        }
        const auto hash = fnv1a64(data, OutputBytes);
        vkUnmapMemory(device, readback.memory);
        std::printf("%s_pixels=%zu/%zu %s_hash=%016llx\n",
                    label, valid, Pixels, label, static_cast<unsigned long long>(hash));
        if (valid != Pixels) throw std::runtime_error("readback contained invalid or unwritten pixels");
    }

    void reportTimings()
    {
        std::array<uint64_t, Samples * 2> ticks{};
        check(vkGetQueryPoolResults(device, timestamps, 0, ticks.size(), sizeof(ticks), ticks.data(),
                                   sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
              "reading GPU timestamps");
        const uint64_t mask = timestampBits < 64 ? (uint64_t{1} << timestampBits) - 1 : UINT64_MAX;
        std::array<double, Samples> milliseconds{};
        for (uint32_t i = 0; i < Samples; ++i) {
            const uint64_t elapsed = (ticks[2 * i + 1] - ticks[2 * i]) & mask;
            if (!elapsed) throw std::runtime_error("GPU timestamp reported a zero-length dispatch");
            milliseconds[i] = static_cast<double>(elapsed) * properties.limits.timestampPeriod / 1.0e6;
        }
        auto sorted = milliseconds;
        std::sort(sorted.begin(), sorted.end());
        const double median = (sorted[(Samples - 1) / 2] + sorted[Samples / 2]) / 2.0;
        constexpr std::size_t P95 = (95 * Samples + 99) / 100 - 1;
        std::printf("gpu_median_ms=%.6f gpu_p95_ms=%.6f\n", median, sorted[P95]);
        std::printf("gpu_samples_ms=");
        for (uint32_t i = 0; i < Samples; ++i) {
            std::printf("%s%.6f", i == 0 ? "" : ",", milliseconds[i]);
        }
        std::puts("");
    }
};
} // namespace

int main()
{
    try {
        Benchmark test;
        if (!test.setup()) {
            std::fputs("Vulkan 1.3 device with FSR 3.1 compute and timestamp support required\n", stderr);
            return 77;
        }
        std::printf("device=%s driver_version=%u queue_family=%u timestamp_valid_bits=%u "
                    "timestamp_compute_and_graphics=%u timestamp_period_ns=%.9g\n",
                    test.properties.deviceName, test.properties.driverVersion, test.queueFamily,
                    test.timestampBits, test.properties.limits.timestampComputeAndGraphics,
                    test.properties.limits.timestampPeriod);
        std::printf("fsr=3.1.4 render=%ux%u output=%ux%u warmup=%u samples=%u hdr=1 auto_exposure=1 "
                    "sharpening=0 frame_generation=0 motion=0 jitter=0 color=0.25,0.5,0.75,1 depth=0.5\n",
                    Render.width, Render.height, Output.width, Output.height, Warmup, Samples);
        test.initializeImages();
        test.dispatchFrame(1, -1, true);
        test.checkReadback("reset");
        for (uint32_t i = 1; i < Warmup; ++i) test.dispatchFrame(i + 1);
        for (uint32_t i = 0; i < Samples; ++i) test.dispatchFrame(Warmup + i + 1, i);
        test.poisonOutput();
        test.dispatchFrame(Warmup + Samples + 1, -1, true);
        test.checkReadback("temporal");
        test.reportTimings();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
