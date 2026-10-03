// SPDX-License-Identifier: MPL-2.0
//
// Linux Vulkan interop. See LinuxVulkanInterop.h.

#include "LinuxVulkanInterop.h"

#if defined(__linux__) && defined(XR_USE_GRAPHICS_API_VULKAN)

#include "VulkanDispatch.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>

const char* const kLinuxVulkanInstanceExtensions =
    "VK_KHR_external_memory_capabilities VK_KHR_external_semaphore_capabilities "
    "VK_KHR_get_physical_device_properties2";
const char* const kLinuxVulkanDeviceExtensions =
    "VK_KHR_dedicated_allocation VK_KHR_get_memory_requirements2 VK_KHR_external_memory "
    "VK_KHR_external_memory_fd VK_KHR_timeline_semaphore VK_KHR_external_semaphore "
    "VK_KHR_external_semaphore_fd";

namespace
{

constexpr uint32_t kPackedSlotCount = 3;
constexpr uint64_t kWaitTimeoutNs = 1'000'000'000ull;

PFN_vkVoidFunction InstanceProc(VkInstance instance, const char* name, const char* fallback = nullptr)
{
    if (gVulkanDispatch.getInstanceProcAddr == nullptr)
    {
        return nullptr;
    }
    PFN_vkVoidFunction fn = gVulkanDispatch.getInstanceProcAddr(instance, name);
    if (fn == nullptr && fallback != nullptr)
    {
        fn = gVulkanDispatch.getInstanceProcAddr(instance, fallback);
    }
    return fn;
}

std::vector<std::string> SplitNames(const char* list)
{
    std::vector<std::string> names;
    std::string all(list);
    size_t start = 0;
    while (start < all.size())
    {
        size_t end = all.find(' ', start);
        if (end == std::string::npos)
        {
            end = all.size();
        }
        names.push_back(all.substr(start, end - start));
        start = end + 1;
    }
    return names;
}

// The typed format a packed image stores; the copy moves sRGB-encoded bytes unchanged, as on Windows.
VkFormat PackedFormat(VkFormat format)
{
    switch (format)
    {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_UNORM:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
            return VK_FORMAT_B8G8R8A8_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

bool IsDepthFormat(VkFormat format)
{
    return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool HasStencil(VkFormat format)
{
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

const LinuxFrameImage* EyeOf(const FrameImageSource& source)
{
    if (!source.IsValid())
    {
        return nullptr;
    }
    const LinuxFrameImage* image = static_cast<const LinuxFrameImage*>(source.GetImage());
    return image->packed ? nullptr : image;
}

struct DeviceFuncs
{
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer resetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkResetFences resetFences = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;

    // Null unless the app enabled the export extensions and timeline semaphores.
    PFN_vkWaitSemaphores waitSemaphores = nullptr;
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;

    bool Load(VkDevice device)
    {
        PFN_vkGetDeviceProcAddr gdpa = gVulkanDispatch.getDeviceProcAddr;
        if (gdpa == nullptr)
        {
            return false;
        }
#define OXR_LOAD(member, name) member = reinterpret_cast<decltype(member)>(gdpa(device, name))
        OXR_LOAD(createImage, "vkCreateImage");
        OXR_LOAD(destroyImage, "vkDestroyImage");
        OXR_LOAD(getImageMemoryRequirements, "vkGetImageMemoryRequirements");
        OXR_LOAD(allocateMemory, "vkAllocateMemory");
        OXR_LOAD(freeMemory, "vkFreeMemory");
        OXR_LOAD(bindImageMemory, "vkBindImageMemory");
        OXR_LOAD(getDeviceQueue, "vkGetDeviceQueue");
        OXR_LOAD(createCommandPool, "vkCreateCommandPool");
        OXR_LOAD(destroyCommandPool, "vkDestroyCommandPool");
        OXR_LOAD(allocateCommandBuffers, "vkAllocateCommandBuffers");
        OXR_LOAD(resetCommandBuffer, "vkResetCommandBuffer");
        OXR_LOAD(beginCommandBuffer, "vkBeginCommandBuffer");
        OXR_LOAD(endCommandBuffer, "vkEndCommandBuffer");
        OXR_LOAD(cmdPipelineBarrier, "vkCmdPipelineBarrier");
        OXR_LOAD(cmdCopyImage, "vkCmdCopyImage");
        OXR_LOAD(queueSubmit, "vkQueueSubmit");
        OXR_LOAD(createFence, "vkCreateFence");
        OXR_LOAD(destroyFence, "vkDestroyFence");
        OXR_LOAD(waitForFences, "vkWaitForFences");
        OXR_LOAD(resetFences, "vkResetFences");
        OXR_LOAD(createSemaphore, "vkCreateSemaphore");
        OXR_LOAD(destroySemaphore, "vkDestroySemaphore");
        OXR_LOAD(waitSemaphores, "vkWaitSemaphores");
        if (waitSemaphores == nullptr)
        {
            OXR_LOAD(waitSemaphores, "vkWaitSemaphoresKHR");
        }
        OXR_LOAD(getMemoryFd, "vkGetMemoryFdKHR");
        OXR_LOAD(getSemaphoreFd, "vkGetSemaphoreFdKHR");
#undef OXR_LOAD
        return createImage && destroyImage && getImageMemoryRequirements && allocateMemory && freeMemory &&
               bindImageMemory && getDeviceQueue && createCommandPool && destroyCommandPool &&
               allocateCommandBuffers && resetCommandBuffer && beginCommandBuffer && endCommandBuffer &&
               cmdPipelineBarrier && cmdCopyImage && queueSubmit && createFence && destroyFence && waitForFences &&
               resetFences && createSemaphore && destroySemaphore;
    }
};

std::atomic<uint64_t> gNextInteropId{1};

} // namespace

struct LinuxPackedSlot
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t generation = 0;
    uint64_t lastValue = 0; // timeline value of the last copy into image
    std::shared_ptr<std::atomic_bool> inUse = std::make_shared<std::atomic_bool>(false);
};

// One per VkDevice: a command pool on the app's queue family, the exportable timeline semaphore
// and the packed images, after the Interop in D3D11Interop.cpp.
struct LinuxVulkanInterop
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    uint64_t id = 0;
    DeviceFuncs vk;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer setupCommandBuffer = VK_NULL_HANDLE;
    VkFence setupFence = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t timelineValue = 0;
    std::string exportUnavailable; // why frames cannot be exported; empty when they can

    std::array<LinuxPackedSlot, kPackedSlotCount> slots;
    uint64_t generationCounter = 0;
    std::mutex mutex; // guards the command buffers, the slots and timelineValue

    ~LinuxVulkanInterop()
    {
        if (device == VK_NULL_HANDLE)
        {
            return;
        }
        WaitValue(timelineValue);
        for (LinuxPackedSlot& slot : slots)
        {
            DestroySlotImage(slot);
        }
        if (timeline != VK_NULL_HANDLE)
        {
            vk.destroySemaphore(device, timeline, nullptr);
        }
        if (setupFence != VK_NULL_HANDLE)
        {
            vk.destroyFence(device, setupFence, nullptr);
        }
        if (commandPool != VK_NULL_HANDLE)
        {
            vk.destroyCommandPool(device, commandPool, nullptr);
        }
    }

    bool Initialize(const VulkanGraphicsContext& ctx)
    {
        instance = static_cast<VkInstance>(ctx.instance);
        physicalDevice = static_cast<VkPhysicalDevice>(ctx.physicalDevice);
        device = static_cast<VkDevice>(ctx.device);
        queueFamilyIndex = ctx.queueFamilyIndex;
        id = gNextInteropId.fetch_add(1);
        if (!vk.Load(device))
        {
            spdlog::error("OXRSys: core Vulkan device functions missing for the Linux interop");
            device = VK_NULL_HANDLE;
            return false;
        }
        vk.getDeviceQueue(device, ctx.queueFamilyIndex, ctx.queueIndex, &queue);

        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = ctx.queueFamilyIndex;
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vk.createCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS ||
            vk.createFence(device, &fenceInfo, nullptr, &setupFence) != VK_SUCCESS)
        {
            spdlog::error("OXRSys: Linux interop command pool creation failed");
            return false;
        }
        std::array<VkCommandBuffer, kPackedSlotCount + 1> buffers = {};
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = static_cast<uint32_t>(buffers.size());
        if (vk.allocateCommandBuffers(device, &allocInfo, buffers.data()) != VK_SUCCESS)
        {
            spdlog::error("OXRSys: Linux interop command buffer allocation failed");
            return false;
        }
        setupCommandBuffer = buffers[0];
        for (uint32_t i = 0; i < kPackedSlotCount; ++i)
        {
            slots[i].commandBuffer = buffers[i + 1];
        }

        exportUnavailable = CreateTimeline();
        if (exportUnavailable.empty())
        {
            spdlog::info("OXRSys: Linux Vulkan interop exports frames on queue family {}", queueFamilyIndex);
        }
        else
        {
            spdlog::warn("OXRSys: Linux streams black frames: {}", exportUnavailable);
        }
        return true;
    }

    std::string CreateTimeline()
    {
        if (vk.getMemoryFd == nullptr || vk.getSemaphoreFd == nullptr || vk.waitSemaphores == nullptr)
        {
            return std::string("the app's VkDevice lacks external fd or timeline semaphore functions; enable ") +
                   kLinuxVulkanDeviceExtensions;
        }
        PFN_vkGetPhysicalDeviceExternalSemaphoreProperties getSemaphoreProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceExternalSemaphoreProperties>(
                InstanceProc(instance, "vkGetPhysicalDeviceExternalSemaphoreProperties",
                             "vkGetPhysicalDeviceExternalSemaphorePropertiesKHR"));
        if (getSemaphoreProperties == nullptr)
        {
            return "vkGetPhysicalDeviceExternalSemaphoreProperties is not available";
        }
        VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkPhysicalDeviceExternalSemaphoreInfo semaphoreInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
                                                            &typeInfo};
        semaphoreInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkExternalSemaphoreProperties semaphoreProperties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
        getSemaphoreProperties(physicalDevice, &semaphoreInfo, &semaphoreProperties);
        if ((semaphoreProperties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT) == 0)
        {
            return "the GPU cannot export a timeline semaphore as an opaque fd";
        }

        VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        typeInfo.pNext = &exportInfo;
        VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &typeInfo};
        if (vk.createSemaphore(device, &createInfo, nullptr, &timeline) != VK_SUCCESS)
        {
            timeline = VK_NULL_HANDLE;
            return "exportable timeline semaphore creation failed";
        }
        return {};
    }

    bool WaitValue(uint64_t value)
    {
        if (value == 0 || timeline == VK_NULL_HANDLE)
        {
            return true;
        }
        VkSemaphoreWaitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &timeline;
        waitInfo.pValues = &value;
        return vk.waitSemaphores(device, &waitInfo, kWaitTimeoutNs) == VK_SUCCESS;
    }

    bool SubmitAndWait(VkCommandBuffer cb)
    {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        if (vk.queueSubmit(queue, 1, &submit, setupFence) != VK_SUCCESS)
        {
            return false;
        }
        VkResult result = vk.waitForFences(device, 1, &setupFence, VK_TRUE, kWaitTimeoutNs);
        vk.resetFences(device, 1, &setupFence);
        return result == VK_SUCCESS;
    }

    void DestroySlotImage(LinuxPackedSlot& slot)
    {
        if (slot.image != VK_NULL_HANDLE)
        {
            vk.destroyImage(device, slot.image, nullptr);
        }
        if (slot.memory != VK_NULL_HANDLE)
        {
            vk.freeMemory(device, slot.memory, nullptr);
        }
        slot.image = VK_NULL_HANDLE;
        slot.memory = VK_NULL_HANDLE;
        slot.format = VK_FORMAT_UNDEFINED;
        slot.width = 0;
        slot.height = 0;
    }

    bool CreateSlotImage(LinuxPackedSlot& slot, VkFormat format, uint32_t width, uint32_t height)
    {
        DestroySlotImage(slot);
        VkImageCreateInfo imageInfo = LinuxPackedImageCreateInfo(format, width, height);

        PFN_vkGetPhysicalDeviceImageFormatProperties2 getFormatProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(InstanceProc(
                instance, "vkGetPhysicalDeviceImageFormatProperties2", "vkGetPhysicalDeviceImageFormatProperties2KHR"));
        VkPhysicalDeviceExternalImageFormatInfo externalFormatInfo{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        externalFormatInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkPhysicalDeviceImageFormatInfo2 formatInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                                    &externalFormatInfo};
        formatInfo.format = format;
        formatInfo.type = imageInfo.imageType;
        formatInfo.tiling = imageInfo.tiling;
        formatInfo.usage = imageInfo.usage;
        formatInfo.flags = imageInfo.flags;
        VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 formatProperties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &externalProperties};
        if (getFormatProperties == nullptr ||
            getFormatProperties(physicalDevice, &formatInfo, &formatProperties) != VK_SUCCESS ||
            (externalProperties.externalMemoryProperties.externalMemoryFeatures &
             VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) == 0)
        {
            spdlog::error("OXRSys: format {} cannot be exported as opaque fd memory", static_cast<int>(format));
            return false;
        }

        VkExternalMemoryImageCreateInfo externalInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        imageInfo.pNext = &externalInfo;
        if (vk.createImage(device, &imageInfo, nullptr, &slot.image) != VK_SUCCESS)
        {
            slot.image = VK_NULL_HANDLE;
            spdlog::error("OXRSys: packed image creation failed");
            return false;
        }

        VkMemoryRequirements requirements = {};
        vk.getImageMemoryRequirements(device, slot.image, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProperties = {};
        gVulkanDispatch.getPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
        uint32_t memoryType = memoryProperties.memoryTypeCount;
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
        {
            if ((requirements.memoryTypeBits & (1u << i)) != 0 &&
                (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0)
            {
                memoryType = i;
                break;
            }
        }
        if (memoryType == memoryProperties.memoryTypeCount)
        {
            DestroySlotImage(slot);
            spdlog::error("OXRSys: no device-local memory type for the packed image");
            return false;
        }

        // Dedicated, because the encoder's import is dedicated and opaque fd imports must match.
        VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedInfo.image = slot.image;
        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicatedInfo};
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo};
        allocateInfo.allocationSize = requirements.size;
        allocateInfo.memoryTypeIndex = memoryType;
        if (vk.allocateMemory(device, &allocateInfo, nullptr, &slot.memory) != VK_SUCCESS ||
            vk.bindImageMemory(device, slot.image, slot.memory, 0) != VK_SUCCESS)
        {
            DestroySlotImage(slot);
            spdlog::error("OXRSys: exportable memory for the packed image failed");
            return false;
        }
        slot.format = format;
        slot.width = width;
        slot.height = height;
        slot.generation = ++generationCounter;
        return true;
    }
};

namespace
{

std::mutex gInteropMutex;
std::shared_ptr<LinuxVulkanInterop> gInterop;
VkDevice gFailedDevice = VK_NULL_HANDLE;

std::shared_ptr<LinuxVulkanInterop> GetInterop(const VulkanGraphicsContext& ctx)
{
    std::scoped_lock lock(gInteropMutex);
    VkDevice device = static_cast<VkDevice>(ctx.device);
    if (gInterop && gInterop->device == device)
    {
        return gInterop;
    }
    if (device == VK_NULL_HANDLE || device == gFailedDevice)
    {
        return nullptr;
    }
    std::shared_ptr<LinuxVulkanInterop> interop = std::make_shared<LinuxVulkanInterop>();
    if (!interop->Initialize(ctx))
    {
        gFailedDevice = device;
        return nullptr;
    }
    gInterop = interop;
    return gInterop;
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageAspectFlags aspect, uint32_t baseLayer, uint32_t layerCount,
                                  VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess,
                                  VkAccessFlags dstAccess)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = baseLayer;
    barrier.subresourceRange.layerCount = layerCount;
    return barrier;
}

void LogOnce(std::atomic_bool& logged, const std::string& message)
{
    if (!logged.exchange(true))
    {
        spdlog::warn("OXRSys: {}", message);
    }
}

} // namespace

std::string LinuxSupportedDeviceExtensions(VkInstance instance, VkPhysicalDevice physicalDevice)
{
    PFN_vkEnumerateDeviceExtensionProperties enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        InstanceProc(instance, "vkEnumerateDeviceExtensionProperties"));
    if (enumerate == nullptr)
    {
        return {};
    }
    uint32_t count = 0;
    enumerate(physicalDevice, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    enumerate(physicalDevice, nullptr, &count, available.data());

    std::string supported;
    for (const std::string& name : SplitNames(kLinuxVulkanDeviceExtensions))
    {
        bool present = std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& extension) {
            return name == extension.extensionName;
        });
        if (!present)
        {
            spdlog::warn("OXRSys: the GPU lacks {}; Linux will stream black frames", name);
            continue;
        }
        supported += supported.empty() ? name : " " + name;
    }
    return supported;
}

bool LinuxTransitionSwapchainImages(const VulkanGraphicsContext& context, const std::vector<uint64_t>& images,
                                    uint32_t arraySize, VkFormat format)
{
    std::shared_ptr<LinuxVulkanInterop> interop = GetInterop(context);
    if (!interop)
    {
        return false;
    }
    const bool depth = IsDepthFormat(format);
    const VkImageAspectFlags aspect =
        depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (HasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
              : VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageLayout layout =
        depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAccessFlags access =
        depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    std::vector<VkImageMemoryBarrier> barriers;
    for (uint64_t handle : images)
    {
        if (handle != 0)
        {
            barriers.push_back(ImageBarrier(reinterpret_cast<VkImage>(handle), aspect, 0, arraySize,
                                            VK_IMAGE_LAYOUT_UNDEFINED, layout, 0, access));
        }
    }
    if (barriers.empty())
    {
        return true;
    }

    std::scoped_lock lock(interop->mutex);
    VkCommandBuffer cb = interop->setupCommandBuffer;
    interop->vk.resetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    interop->vk.beginCommandBuffer(cb, &beginInfo);
    interop->vk.cmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                   nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
    interop->vk.endCommandBuffer(cb);
    return interop->SubmitAndWait(cb);
}

FrameImageSource LinuxEyeSource(VkImage image, uint32_t arrayLayer, uint32_t width, uint32_t height, VkFormat format)
{
    LinuxFrameImage* eye = new LinuxFrameImage{};
    eye->image = image;
    eye->arrayLayer = arrayLayer;
    eye->width = width;
    eye->height = height;
    eye->format = format;
    FrameImageSource source = {};
    source.api = GraphicsApi::Vulkan;
    source.image = std::shared_ptr<void>(eye, [](void* ptr) { delete static_cast<LinuxFrameImage*>(ptr); });
    return source;
}

VkImageCreateInfo LinuxPackedImageCreateInfo(VkFormat format, uint32_t width, uint32_t height)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

void LinuxPackVulkanFrame(const VulkanGraphicsContext& context, FrameSource& frameSource)
{
    static std::atomic_bool loggedFormat{false};
    static std::atomic_bool loggedSlot{false};
    static std::atomic_bool loggedSubmit{false};

    const LinuxFrameImage* left = EyeOf(frameSource.left);
    const LinuxFrameImage* right = EyeOf(frameSource.right);
    if (left == nullptr || right == nullptr)
    {
        return;
    }
    std::shared_ptr<LinuxVulkanInterop> interop = GetInterop(context);
    if (!interop || !interop->exportUnavailable.empty())
    {
        return;
    }
    const VkFormat format = PackedFormat(left->format);
    if (format == VK_FORMAT_UNDEFINED || PackedFormat(right->format) != format)
    {
        LogOnce(loggedFormat, "swapchain formats " + std::to_string(left->format) + "/" +
                                  std::to_string(right->format) + " are not streamed; Linux streams black frames");
        return;
    }
    const uint32_t eyeWidth = left->width;
    const uint32_t eyeHeight = left->height;

    std::scoped_lock lock(interop->mutex);
    LinuxPackedSlot* slot = nullptr;
    uint32_t slotIndex = 0;
    for (uint32_t i = 0; i < kPackedSlotCount; ++i)
    {
        bool expected = false;
        if (interop->slots[i].inUse->compare_exchange_strong(expected, true))
        {
            slot = &interop->slots[i];
            slotIndex = i;
            break;
        }
    }
    if (slot == nullptr)
    {
        frameSource.left.Reset();
        frameSource.right.Reset();
        return; // the encoder still holds every packed image; skip this frame
    }
    if (!interop->WaitValue(slot->lastValue))
    {
        slot->inUse->store(false);
        frameSource.left.Reset();
        frameSource.right.Reset();
        return;
    }
    if ((slot->format != format || slot->width != eyeWidth * 2 || slot->height != eyeHeight) &&
        !interop->CreateSlotImage(*slot, format, eyeWidth * 2, eyeHeight))
    {
        slot->inUse->store(false);
        LogOnce(loggedSlot, "no exportable packed image; Linux streams black frames");
        return;
    }

    const DeviceFuncs& vk = interop->vk;
    VkCommandBuffer cb = slot->commandBuffer;
    vk.resetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.beginCommandBuffer(cb, &beginInfo);

    // The app releases eyes in COLOR_ATTACHMENT_OPTIMAL (XR_KHR_vulkan_enable2) and gets them back so.
    const bool sameSubresource = left->image == right->image && left->arrayLayer == right->arrayLayer;
    std::vector<VkImageMemoryBarrier> before;
    std::vector<VkImageMemoryBarrier> after;
    for (const LinuxFrameImage* eye : {left, right})
    {
        if (eye == right && sameSubresource)
        {
            break;
        }
        before.push_back(ImageBarrier(eye->image, VK_IMAGE_ASPECT_COLOR_BIT, eye->arrayLayer, 1,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                      VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT));
        after.push_back(ImageBarrier(eye->image, VK_IMAGE_ASPECT_COLOR_BIT, eye->arrayLayer, 1,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT));
    }
    before.push_back(ImageBarrier(slot->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT));
    // PyroWave acquires external images in GENERAL from VK_QUEUE_FAMILY_EXTERNAL.
    VkImageMemoryBarrier release =
        ImageBarrier(slot->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    release.srcQueueFamilyIndex = interop->queueFamilyIndex;
    release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    after.push_back(release);

    vk.cmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                          nullptr, static_cast<uint32_t>(before.size()), before.data());
    std::array<VkImageCopy, 2> regions = {};
    for (uint32_t i = 0; i < 2; ++i)
    {
        const LinuxFrameImage* eye = i == 0 ? left : right;
        VkImageCopy& region = regions[i];
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye->arrayLayer, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffset = {static_cast<int32_t>(i * eyeWidth), 0, 0};
        region.extent = {std::min(eye->width, eyeWidth), std::min(eye->height, eyeHeight), 1};
        vk.cmdCopyImage(cb, eye->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot->image,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    vk.cmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                          nullptr, static_cast<uint32_t>(after.size()), after.data());
    vk.endCommandBuffer(cb);

    const uint64_t value = interop->timelineValue + 1;
    VkTimelineSemaphoreSubmitInfo timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timelineInfo.signalSemaphoreValueCount = 1;
    timelineInfo.pSignalSemaphoreValues = &value;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timelineInfo};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &interop->timeline;
    if (vk.queueSubmit(interop->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
    {
        slot->inUse->store(false);
        LogOnce(loggedSubmit, "submitting the eye copy failed; Linux streams black frames");
        return;
    }
    interop->timelineValue = value;
    slot->lastValue = value;

    LinuxFrameImage* packed = new LinuxFrameImage{};
    packed->packed = true;
    packed->image = slot->image;
    packed->width = slot->width;
    packed->height = slot->height;
    packed->format = format;
    packed->interop = interop.get();
    packed->slot = slot;
    packed->interopId = interop->id;
    packed->slotIndex = slotIndex;
    packed->slotGeneration = slot->generation;
    packed->timelineValue = value;

    FrameImageSource source = {};
    source.api = GraphicsApi::Vulkan;
    std::shared_ptr<std::atomic_bool> inUse = slot->inUse;
    source.image = std::shared_ptr<void>(packed, [inUse, interop](void* ptr) {
        delete static_cast<LinuxFrameImage*>(ptr);
        inUse->store(false);
    });
    frameSource.left = source;
    frameSource.right = source;
}

int LinuxExportPackedImageFd(const LinuxFrameImage& frame)
{
    if (!frame.packed || frame.interop == nullptr || frame.slot == nullptr)
    {
        return -1;
    }
    VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    info.memory = frame.slot->memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (frame.interop->vk.getMemoryFd(frame.interop->device, &info, &fd) != VK_SUCCESS)
    {
        return -1;
    }
    return fd;
}

int LinuxExportTimelineFd(const LinuxFrameImage& frame)
{
    if (!frame.packed || frame.interop == nullptr || frame.interop->timeline == VK_NULL_HANDLE)
    {
        return -1;
    }
    VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    info.semaphore = frame.interop->timeline;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (frame.interop->vk.getSemaphoreFd(frame.interop->device, &info, &fd) != VK_SUCCESS)
    {
        return -1;
    }
    return fd;
}

bool LinuxGetDeviceUuids(const VulkanGraphicsContext& context, uint8_t deviceUuid[VK_UUID_SIZE],
                         uint8_t driverUuid[VK_UUID_SIZE])
{
    VkInstance instance = static_cast<VkInstance>(context.instance);
    PFN_vkGetPhysicalDeviceProperties2 getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        InstanceProc(instance, "vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceProperties2KHR"));
    if (getProperties2 == nullptr || context.physicalDevice == nullptr)
    {
        return false;
    }
    VkPhysicalDeviceIDProperties idProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idProperties};
    getProperties2(static_cast<VkPhysicalDevice>(context.physicalDevice), &properties);
    std::memcpy(deviceUuid, idProperties.deviceUUID, VK_UUID_SIZE);
    std::memcpy(driverUuid, idProperties.driverUUID, VK_UUID_SIZE);
    return true;
}

#endif
