// SPDX-License-Identifier: MPL-2.0
//
// Linux Vulkan interop, the counterpart of D3D11Interop.h. At xrEndFrame the app's queue copies
// both eyes into one packed image whose memory is exported as an opaque fd, releases it to
// VK_QUEUE_FAMILY_EXTERNAL and signals an exportable timeline semaphore; the PyroWave encoder
// (PyroWaveVideoEncoderLinux.cpp) imports both on its own device on the same GPU.

#pragma once

#if defined(__linux__) && defined(XR_USE_GRAPHICS_API_VULKAN)

#include "GraphicsTypes.h"

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

// Extensions the interop needs; the v1 path reports these, the v2 path enables the supported ones.
extern const char* const kLinuxVulkanInstanceExtensions;
extern const char* const kLinuxVulkanDeviceExtensions;

struct LinuxVulkanInterop;
struct LinuxPackedSlot;

// An eye of a swapchain as released, or the packed side-by-side image built from both eyes.
struct LinuxFrameImage
{
    bool packed = false;
    VkImage image = VK_NULL_HANDLE; // on the app's VkDevice
    uint32_t arrayLayer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;

    // Packed images only.
    LinuxVulkanInterop* interop = nullptr;
    LinuxPackedSlot* slot = nullptr;
    uint64_t interopId = 0;
    uint32_t slotIndex = 0;
    uint64_t slotGeneration = 0;
    uint64_t timelineValue = 0;
};

// The supported subset of kLinuxVulkanDeviceExtensions on physicalDevice, space-separated.
std::string LinuxSupportedDeviceExtensions(VkInstance instance, VkPhysicalDevice physicalDevice);

// Transition fresh swapchain images to the attachment layout the app receives them in.
bool LinuxTransitionSwapchainImages(const VulkanGraphicsContext& context, const std::vector<uint64_t>& images,
                                    uint32_t arraySize, VkFormat format);

FrameImageSource LinuxEyeSource(VkImage image, uint32_t arrayLayer, uint32_t width, uint32_t height,
                                VkFormat format);

// Pack frameSource's two eyes on the app's queue. On success both eyes become the packed image;
// when every packed image is still leased both are cleared; when export is unavailable (logged
// once) the eyes are left as they are and the encoder streams black.
void LinuxPackVulkanFrame(const VulkanGraphicsContext& context, FrameSource& frameSource);

// The image create info both devices use for a packed image; the importer adds its own pNext.
VkImageCreateInfo LinuxPackedImageCreateInfo(VkFormat format, uint32_t width, uint32_t height);

// New opaque fds owned by the caller, or -1. frame must be packed and still leased.
int LinuxExportPackedImageFd(const LinuxFrameImage& frame);
int LinuxExportTimelineFd(const LinuxFrameImage& frame);

// deviceUUID and driverUUID of the app's physical device.
bool LinuxGetDeviceUuids(const VulkanGraphicsContext& context, uint8_t deviceUuid[VK_UUID_SIZE],
                         uint8_t driverUuid[VK_UUID_SIZE]);

#endif
