#pragma once

#include "vulkan_ContextImpl.h"

#include <vector>

#if defined(OS_LINUX) && !defined(VK_USE_PLATFORM_XLIB_KHR)
#define VK_USE_PLATFORM_XLIB_KHR 1
#endif

#if defined(OS_WINDOWS) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR 1
#endif

#ifndef REALITYVK_VULKAN_HEADERS_AVAILABLE
#if __has_include(<vulkan/vulkan.h>)
#define REALITYVK_VULKAN_HEADERS_AVAILABLE 1
#include <vulkan/vulkan.h>
#else
#define REALITYVK_VULKAN_HEADERS_AVAILABLE 0
#endif
#endif

#include "vulkan_BindingState.h"
#include "vulkan_DescriptorBinder.h"
#include "vulkan_DescriptorLayoutRegistry.h"
#include "vulkan_DrawRecorder.h"
#include "vulkan_FramebufferStore.h"
#include "vulkan_PipelineCache.h"
#include "vulkan_ProgramLibrary.h"
#include "vulkan_TextureStore.h"
#include "vulkan_UploadArena.h"

namespace vulkan {

struct ContextImpl::VulkanState
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	struct FrameSync {
		VkSemaphore imageAvailable = VK_NULL_HANDLE;
		VkSemaphore renderFinished = VK_NULL_HANDLE;
		VkFence inFlight = VK_NULL_HANDLE;
	};

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue graphicsQueue = VK_NULL_HANDLE;
	VkQueue presentQueue = VK_NULL_HANDLE;
	u32 graphicsQueueFamily = UINT32_MAX;
	u32 presentQueueFamily = UINT32_MAX;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D swapchainExtent = {};
	VkFormat depthFormat = VK_FORMAT_UNDEFINED;
	std::vector<VkImage> swapchainImages;
	std::vector<VkImageView> swapchainImageViews;
	std::vector<VkImage> swapchainDepthImages;
	std::vector<VkDeviceMemory> swapchainDepthMemories;
	std::vector<VkImageView> swapchainDepthImageViews;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	std::vector<VkFramebuffer> swapchainFramebuffers;
	std::vector<VkFence> swapchainImageFences;
	VkCommandPool commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> frameCommandBuffers;
	std::vector<FrameSync> frameSync;
	DrawRecorder drawRecorder;
	TextureStore textureStore;
	FramebufferStore framebufferStore;
	BindingState bindingState;
	UploadArena uploadArena;
	DescriptorLayoutRegistry descriptorLayoutRegistry;
	DescriptorBinder descriptorBinder;
	ProgramLibrary programLibrary;
	PipelineCache pipelineCache;
	u32 frameSyncIndex = 0;
	u32 lastPresentedImageIndex = UINT32_MAX;
	graphics::ObjectHandle lastNonDefaultDrawFramebuffer = graphics::ObjectHandle::null;
	graphics::ObjectHandle lastNonDefaultReadFramebuffer = graphics::ObjectHandle::null;
	u64 framebufferBindSerial = 0U;
	u64 lastNonDefaultDrawFramebufferSerial = 0U;
	u64 lastNonDefaultReadFramebufferSerial = 0U;
	graphics::ObjectHandle lastColorBlitDrawFramebuffer = graphics::ObjectHandle::null;
	u64 lastColorBlitDrawFramebufferSerial = 0U;
	bool wideLinesEnabled = false;
	bool dualSrcBlendEnabled = false;
	bool surfaceExtensionEnabled = false;
	bool xlibSurfaceExtensionEnabled = false;
	bool win32SurfaceExtensionEnabled = false;
	bool swapchainExtensionEnabled = false;
#endif
};

#if REALITYVK_VULKAN_HEADERS_AVAILABLE

constexpr VkDeviceSize kInitialFrameVertexBufferSize = 64U * 1024U;

inline u32 findMemoryTypeIndex(VkPhysicalDevice _physicalDevice, u32 _typeFilter, VkMemoryPropertyFlags _requiredProperties)
{
	VkPhysicalDeviceMemoryProperties memoryProperties{};
	vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &memoryProperties);
	for (u32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
		if ((_typeFilter & (1U << i)) == 0U)
			continue;
		if ((memoryProperties.memoryTypes[i].propertyFlags & _requiredProperties) == _requiredProperties)
			return i;
	}
	return UINT32_MAX;
}

inline bool hasStencilComponent(VkFormat _format)
{
	return _format == VK_FORMAT_D24_UNORM_S8_UINT || _format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

inline bool hasDepthComponent(VkFormat _format)
{
	switch (_format) {
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return true;
	default:
		return false;
	}
}

inline VkFormat pickDepthFormat(VkPhysicalDevice _physicalDevice)
{
	const VkFormat candidates[] = {
		VK_FORMAT_D32_SFLOAT,
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D16_UNORM
	};
	for (VkFormat format : candidates) {
		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(_physicalDevice, format, &properties);
		if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
			return format;
	}
	return VK_FORMAT_UNDEFINED;
}

#endif

} // namespace vulkan
