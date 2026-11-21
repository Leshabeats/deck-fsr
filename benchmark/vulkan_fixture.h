/*
 * Copyright (c) 2026 Q2RTX FSR Vulkan contributors
 * SPDX-License-Identifier: MIT
 */

/* Resource helpers adapted from FSR-Vulkan tests/fsr3_vk_backend_smoke.cpp. */
#pragma once

std::uint32_t findMemoryType(VkPhysicalDevice physicalDevice,
                             std::uint32_t allowedTypes,
                             VkMemoryPropertyFlags required,
                             VkMemoryPropertyFlags preferred = 0)
{
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);

    std::uint32_t fallback = UINT32_MAX;
    for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
        if (!(allowedTypes & (1u << index))) {
            continue;
        }
        const VkMemoryPropertyFlags flags = memoryProperties.memoryTypes[index].propertyFlags;
        // This smoke device deliberately leaves deviceCoherentMemory disabled.
        // Keep harness allocations legal so the audit isolates backend choices.
        if (flags & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD) {
            continue;
        }
        if ((flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            return index;
        }
        if (fallback == UINT32_MAX) {
            fallback = index;
        }
    }
    return fallback;
}

struct TestImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageCreateInfo createInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
};

bool createImage(VkPhysicalDevice physicalDevice,
                 VkDevice device,
                 VkFormat format,
                 std::uint32_t width,
                 std::uint32_t height,
                 VkImageUsageFlags usage,
                 TestImage* output)
{
    output->createInfo.imageType = VK_IMAGE_TYPE_2D;
    output->createInfo.format = format;
    output->createInfo.extent = {width, height, 1};
    output->createInfo.mipLevels = 1;
    output->createInfo.arrayLayers = 1;
    output->createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    output->createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    output->createInfo.usage = usage;
    output->createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    output->createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &output->createInfo, nullptr, &output->image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, output->image, &requirements);
    const std::uint32_t memoryType = findMemoryType(
        physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryType == UINT32_MAX) {
        vkDestroyImage(device, output->image, nullptr);
        output->image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device, &allocation, nullptr, &output->memory) != VK_SUCCESS) {
        vkDestroyImage(device, output->image, nullptr);
        output->image = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(device, output->image, output->memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, output->memory, nullptr);
        vkDestroyImage(device, output->image, nullptr);
        output->memory = VK_NULL_HANDLE;
        output->image = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

struct ReadbackBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocationSize = 0;
    VkMemoryPropertyFlags memoryProperties = 0;
};

bool createReadbackBuffer(VkPhysicalDevice physicalDevice,
                          VkDevice device,
                          VkDeviceSize size,
                          ReadbackBuffer* output)
{
    VkBufferCreateInfo createInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    createInfo.size = size;
    createInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &createInfo, nullptr, &output->buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, output->buffer, &requirements);
    const std::uint32_t memoryType = findMemoryType(
        physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (memoryType == UINT32_MAX) {
        vkDestroyBuffer(device, output->buffer, nullptr);
        output->buffer = VK_NULL_HANDLE;
        return false;
    }
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    output->memoryProperties = memoryProperties.memoryTypes[memoryType].propertyFlags;
    output->allocationSize = requirements.size;

    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (vkAllocateMemory(device, &allocation, nullptr, &output->memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, output->buffer, nullptr);
        output->buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(device, output->buffer, output->memory, 0) != VK_SUCCESS) {
        vkFreeMemory(device, output->memory, nullptr);
        vkDestroyBuffer(device, output->buffer, nullptr);
        output->memory = VK_NULL_HANDLE;
        output->buffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void destroyImage(VkDevice device, TestImage* image)
{
    if (image->image) vkDestroyImage(device, image->image, nullptr);
    if (image->memory) vkFreeMemory(device, image->memory, nullptr);
    *image = {};
}

void destroyReadbackBuffer(VkDevice device, ReadbackBuffer* buffer)
{
    if (buffer->buffer) vkDestroyBuffer(device, buffer->buffer, nullptr);
    if (buffer->memory) vkFreeMemory(device, buffer->memory, nullptr);
    *buffer = {};
}

VkImageMemoryBarrier imageBarrier(VkImage image,
                                  VkImageLayout oldLayout,
                                  VkImageLayout newLayout,
                                  VkAccessFlags sourceAccess,
                                  VkAccessFlags destinationAccess)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    return barrier;
}

FfxVkPortableImage asPortableImage(const TestImage& image,
                                   FfxVkPortableResourceState state)
{
    FfxVkPortableImage resource{};
    resource.structSize = sizeof(resource);
    resource.image = image.image;
    resource.format = image.createInfo.format;
    resource.extent = {image.createInfo.extent.width, image.createInfo.extent.height};
    resource.mipCount = image.createInfo.mipLevels;
    resource.arrayLayers = image.createInfo.arrayLayers;
    resource.usage = image.createInfo.usage;
    resource.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    resource.state = state;
    return resource;
}

std::uint64_t fnv1a64(const void* data, std::size_t size)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}
