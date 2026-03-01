#include "vulkan_ContextImpl_Internal.h"

#include <algorithm>
#include <limits>
#include <vector>
#include <Log.h>
#include "vulkan_BasicColorShaders.h"
#include "vulkan_BasicTexturedShaders.h"

namespace vulkan {

bool ContextImpl::createDrawResources()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || m_vk->renderPass == VK_NULL_HANDLE)
		return false;

	destroyDrawResources();
	if (!createShaderModules())
		return false;
	if (!createPipelines()) {
		destroyDrawResources();
		return false;
	}
	m_vk->drawRecorder.markAllStateDirty();
	return true;
#endif
}

bool ContextImpl::createShaderModules()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return false;
	if (!m_vk->descriptorLayoutRegistry.init(m_vk->device))
		return false;
	if (!m_vk->programLibrary.init(m_vk->device, &m_vk->descriptorLayoutRegistry))
		return false;
	if (!m_vk->programLibrary.loadBasicColorProgram(
		kBasicColorVertSpv,
		kBasicColorVertSpv_len,
		kBasicColorFragSpv,
		kBasicColorFragSpv_len)) {
		return false;
	}
	if (!m_vk->programLibrary.loadBasicTexturedProgram(
		kBasicTexturedVertSpv,
		kBasicTexturedVertSpv_len,
		kBasicTexturedFragSpv,
		kBasicTexturedFragSpv_len)) {
		return false;
	}
	return true;
#endif
}

bool ContextImpl::createPipelines()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || m_vk->renderPass == VK_NULL_HANDLE)
		return false;
	const VkShaderModule colorVertexShader = m_vk->programLibrary.getBasicColorVertexShader();
	const VkShaderModule colorFragmentShader = m_vk->programLibrary.getBasicColorFragmentShader();
	const VkShaderModule texturedVertexShader = m_vk->programLibrary.getBasicTexturedVertexShader();
	const VkShaderModule texturedFragmentShader = m_vk->programLibrary.getBasicTexturedFragmentShader();
	const VkPipelineLayout colorPipelineLayout = m_vk->programLibrary.getBasicColorPipelineLayout();
	if (colorVertexShader == VK_NULL_HANDLE || colorFragmentShader == VK_NULL_HANDLE || colorPipelineLayout == VK_NULL_HANDLE)
		return false;
	if (!m_vk->pipelineCache.init(
		m_vk->device,
		m_vk->renderPass,
		colorPipelineLayout,
		colorVertexShader,
		colorFragmentShader,
		texturedVertexShader,
		texturedFragmentShader)) {
		return false;
	}
	if (!m_vk->descriptorBinder.init(
		m_vk->device,
		&m_vk->descriptorLayoutRegistry,
		&m_vk->textureStore,
		static_cast<u32>(std::max<size_t>(1, m_vk->frameSync.size())))) {
		m_vk->pipelineCache.destroy();
		return false;
	}
	return true;
#endif
}

void ContextImpl::destroyDrawResources()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return;

	m_vk->pipelineCache.destroy();
	m_vk->descriptorBinder.destroy();
	m_vk->programLibrary.destroy();
	m_vk->descriptorLayoutRegistry.destroy();
#endif
}

void ContextImpl::resetFrameRenderData()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.resetFramePackets();
#endif
}

bool ContextImpl::isDefaultDrawFramebufferBound() const
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk)
		return false;
	return m_vk->drawRecorder.isDefaultDrawFramebufferBound();
#endif
}

void ContextImpl::createSwapchain()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || !m_vk->swapchainExtensionEnabled)
		return;
	if (m_vk->physicalDevice == VK_NULL_HANDLE)
		return;
	if (m_vk->graphicsQueueFamily == UINT32_MAX || m_vk->presentQueueFamily == UINT32_MAX)
		return;
	if (m_vk->surface == VK_NULL_HANDLE) {
		LOG(LOG_WARNING, "Vulkan surface is not initialized yet; swapchain creation is deferred.");
		return;
	}

	if (m_vk->swapchain != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(m_vk->device);
		destroySwapchain();
	}

	VkSurfaceCapabilitiesKHR capabilities{};
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_vk->physicalDevice, m_vk->surface, &capabilities) != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed.");
		return;
	}

	if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
		LOG(LOG_WARNING, "Vulkan surface does not support color-attachment swapchain images.");
		return;
	}

	u32 formatCount = 0;
	if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_vk->physicalDevice, m_vk->surface, &formatCount, nullptr) != VK_SUCCESS || formatCount == 0) {
		LOG(LOG_WARNING, "vkGetPhysicalDeviceSurfaceFormatsKHR failed.");
		return;
	}

	std::vector<VkSurfaceFormatKHR> formats(formatCount);
	if (vkGetPhysicalDeviceSurfaceFormatsKHR(m_vk->physicalDevice, m_vk->surface, &formatCount, formats.data()) != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkGetPhysicalDeviceSurfaceFormatsKHR failed.");
		return;
	}

	VkSurfaceFormatKHR selectedFormat = formats[0];
	for (const auto & format : formats) {
		if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			selectedFormat = format;
			break;
		}
		if (format.format == VK_FORMAT_R8G8B8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			selectedFormat = format;
		}
	}

	u32 presentModeCount = 0;
	std::vector<VkPresentModeKHR> presentModes;
	if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_vk->physicalDevice, m_vk->surface, &presentModeCount, nullptr) == VK_SUCCESS && presentModeCount > 0) {
		presentModes.resize(presentModeCount);
		if (vkGetPhysicalDeviceSurfacePresentModesKHR(m_vk->physicalDevice, m_vk->surface, &presentModeCount, presentModes.data()) != VK_SUCCESS) {
			presentModes.clear();
		}
	}

	VkPresentModeKHR selectedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
	for (VkPresentModeKHR presentMode : presentModes) {
		if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
			selectedPresentMode = presentMode;
			break;
		}
	}

	VkExtent2D extent = capabilities.currentExtent;
	if (extent.width == std::numeric_limits<u32>::max()) {
		const u32 requestedWidth = std::max<u32>(1U, m_presentationWindowInfo.width);
		const u32 requestedHeight = std::max<u32>(1U, m_presentationWindowInfo.height);
		extent.width = std::min(capabilities.maxImageExtent.width, std::max(capabilities.minImageExtent.width, requestedWidth));
		extent.height = std::min(capabilities.maxImageExtent.height, std::max(capabilities.minImageExtent.height, requestedHeight));
	}

	u32 imageCount = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount)
		imageCount = capabilities.maxImageCount;

	VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if ((capabilities.supportedCompositeAlpha & compositeAlpha) == 0) {
		if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) {
			compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
		} else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) {
			compositeAlpha = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
		} else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
			compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
		}
	}

	VkSwapchainCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = m_vk->surface;
	createInfo.minImageCount = imageCount;
	createInfo.imageFormat = selectedFormat.format;
	createInfo.imageColorSpace = selectedFormat.colorSpace;
	createInfo.imageExtent = extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	u32 queueFamilyIndices[2] = { m_vk->graphicsQueueFamily, m_vk->presentQueueFamily };
	if (m_vk->graphicsQueueFamily != m_vk->presentQueueFamily) {
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = queueFamilyIndices;
	} else {
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		createInfo.queueFamilyIndexCount = 0;
		createInfo.pQueueFamilyIndices = nullptr;
	}
	createInfo.preTransform = capabilities.currentTransform;
	createInfo.compositeAlpha = compositeAlpha;
	createInfo.presentMode = selectedPresentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = VK_NULL_HANDLE;

	const VkResult result = vkCreateSwapchainKHR(m_vk->device, &createInfo, nullptr, &m_vk->swapchain);
	if (result != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkCreateSwapchainKHR failed: %d", static_cast<int>(result));
		return;
	}

	u32 swapchainImageCount = 0;
	if (vkGetSwapchainImagesKHR(m_vk->device, m_vk->swapchain, &swapchainImageCount, nullptr) != VK_SUCCESS || swapchainImageCount == 0U) {
		LOG(LOG_WARNING, "vkGetSwapchainImagesKHR failed.");
		destroySwapchain();
		return;
	}

	m_vk->swapchainImages.resize(swapchainImageCount, VK_NULL_HANDLE);
	if (vkGetSwapchainImagesKHR(m_vk->device, m_vk->swapchain, &swapchainImageCount, m_vk->swapchainImages.data()) != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkGetSwapchainImagesKHR failed.");
		destroySwapchain();
		return;
	}

	m_vk->swapchainImageViews.clear();
	m_vk->swapchainImageViews.reserve(m_vk->swapchainImages.size());
	for (VkImage image : m_vk->swapchainImages) {
		VkImageViewCreateInfo imageViewCreateInfo{};
		imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		imageViewCreateInfo.image = image;
		imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageViewCreateInfo.format = selectedFormat.format;
		imageViewCreateInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
		imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
		imageViewCreateInfo.subresourceRange.levelCount = 1;
		imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
		imageViewCreateInfo.subresourceRange.layerCount = 1;

		VkImageView imageView = VK_NULL_HANDLE;
		if (vkCreateImageView(m_vk->device, &imageViewCreateInfo, nullptr, &imageView) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateImageView failed for swapchain image.");
			destroySwapchain();
			return;
		}
		m_vk->swapchainImageViews.push_back(imageView);
	}

	m_vk->depthFormat = pickDepthFormat(m_vk->physicalDevice);
	if (m_vk->depthFormat == VK_FORMAT_UNDEFINED) {
		LOG(LOG_WARNING, "No supported Vulkan depth format found.");
		destroySwapchain();
		return;
	}

	const bool depthHasStencil = hasStencilComponent(m_vk->depthFormat);
	const VkImageAspectFlags depthAspectMask = depthHasStencil
		? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
		: VK_IMAGE_ASPECT_DEPTH_BIT;
	m_vk->swapchainDepthImages.assign(m_vk->swapchainImages.size(), VK_NULL_HANDLE);
	m_vk->swapchainDepthMemories.assign(m_vk->swapchainImages.size(), VK_NULL_HANDLE);
	m_vk->swapchainDepthImageViews.assign(m_vk->swapchainImages.size(), VK_NULL_HANDLE);

	for (size_t i = 0; i < m_vk->swapchainDepthImages.size(); ++i) {
		VkImageCreateInfo depthImageCreateInfo{};
		depthImageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		depthImageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		depthImageCreateInfo.extent.width = extent.width;
		depthImageCreateInfo.extent.height = extent.height;
		depthImageCreateInfo.extent.depth = 1;
		depthImageCreateInfo.mipLevels = 1;
		depthImageCreateInfo.arrayLayers = 1;
		depthImageCreateInfo.format = m_vk->depthFormat;
		depthImageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		depthImageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		depthImageCreateInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		depthImageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		depthImageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

		if (vkCreateImage(m_vk->device, &depthImageCreateInfo, nullptr, &m_vk->swapchainDepthImages[i]) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateImage failed for swapchain depth image.");
			destroySwapchain();
			return;
		}

		VkMemoryRequirements depthMemoryRequirements{};
		vkGetImageMemoryRequirements(m_vk->device, m_vk->swapchainDepthImages[i], &depthMemoryRequirements);
		const u32 depthMemoryTypeIndex = findMemoryTypeIndex(
			m_vk->physicalDevice,
			depthMemoryRequirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (depthMemoryTypeIndex == UINT32_MAX) {
			LOG(LOG_WARNING, "No compatible memory type found for Vulkan depth image.");
			destroySwapchain();
			return;
		}

		VkMemoryAllocateInfo depthMemoryAllocateInfo{};
		depthMemoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		depthMemoryAllocateInfo.allocationSize = depthMemoryRequirements.size;
		depthMemoryAllocateInfo.memoryTypeIndex = depthMemoryTypeIndex;
		if (vkAllocateMemory(m_vk->device, &depthMemoryAllocateInfo, nullptr, &m_vk->swapchainDepthMemories[i]) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkAllocateMemory failed for swapchain depth image.");
			destroySwapchain();
			return;
		}

		if (vkBindImageMemory(m_vk->device, m_vk->swapchainDepthImages[i], m_vk->swapchainDepthMemories[i], 0) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkBindImageMemory failed for swapchain depth image.");
			destroySwapchain();
			return;
		}

		VkImageViewCreateInfo depthImageViewCreateInfo{};
		depthImageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		depthImageViewCreateInfo.image = m_vk->swapchainDepthImages[i];
		depthImageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		depthImageViewCreateInfo.format = m_vk->depthFormat;
		depthImageViewCreateInfo.subresourceRange.aspectMask = depthAspectMask;
		depthImageViewCreateInfo.subresourceRange.baseMipLevel = 0;
		depthImageViewCreateInfo.subresourceRange.levelCount = 1;
		depthImageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
		depthImageViewCreateInfo.subresourceRange.layerCount = 1;

		if (vkCreateImageView(m_vk->device, &depthImageViewCreateInfo, nullptr, &m_vk->swapchainDepthImageViews[i]) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateImageView failed for swapchain depth image.");
			destroySwapchain();
			return;
		}
	}

	const VkAttachmentDescription attachments[2] = {
		{
			0,
			selectedFormat.format,
			VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_STORE_OP_STORE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
		},
		{
			0,
			m_vk->depthFormat,
			VK_SAMPLE_COUNT_1_BIT,
			VK_ATTACHMENT_LOAD_OP_CLEAR,
			VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			VK_ATTACHMENT_STORE_OP_DONT_CARE,
			VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
		}
	};

	VkAttachmentReference colorAttachmentReference{};
	colorAttachmentReference.attachment = 0;
	colorAttachmentReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depthAttachmentReference{};
	depthAttachmentReference.attachment = 1;
	depthAttachmentReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpassDescription{};
	subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpassDescription.colorAttachmentCount = 1;
	subpassDescription.pColorAttachments = &colorAttachmentReference;
	subpassDescription.pDepthStencilAttachment = &depthAttachmentReference;

	VkSubpassDependency subpassDependency{};
	subpassDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	subpassDependency.dstSubpass = 0;
	subpassDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
		| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
		| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	subpassDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
		| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
		| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	subpassDependency.srcAccessMask = 0;
	subpassDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
		| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	VkRenderPassCreateInfo renderPassCreateInfo{};
	renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	renderPassCreateInfo.attachmentCount = 2;
	renderPassCreateInfo.pAttachments = attachments;
	renderPassCreateInfo.subpassCount = 1;
	renderPassCreateInfo.pSubpasses = &subpassDescription;
	renderPassCreateInfo.dependencyCount = 1;
	renderPassCreateInfo.pDependencies = &subpassDependency;
	if (vkCreateRenderPass(m_vk->device, &renderPassCreateInfo, nullptr, &m_vk->renderPass) != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkCreateRenderPass failed for swapchain.");
		destroySwapchain();
		return;
	}
	if (!createDrawResources()) {
		LOG(LOG_WARNING, "Failed to initialize Vulkan draw resources.");
		destroySwapchain();
		return;
	}

	m_vk->swapchainFramebuffers.clear();
	m_vk->swapchainFramebuffers.reserve(m_vk->swapchainImageViews.size());
	for (size_t i = 0; i < m_vk->swapchainImageViews.size(); ++i) {
		const VkImageView attachmentsForFramebuffer[2] = {
			m_vk->swapchainImageViews[i],
			m_vk->swapchainDepthImageViews[i]
		};
		VkFramebufferCreateInfo framebufferCreateInfo{};
		framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferCreateInfo.renderPass = m_vk->renderPass;
		framebufferCreateInfo.attachmentCount = 2;
		framebufferCreateInfo.pAttachments = attachmentsForFramebuffer;
		framebufferCreateInfo.width = extent.width;
		framebufferCreateInfo.height = extent.height;
		framebufferCreateInfo.layers = 1;

		VkFramebuffer framebuffer = VK_NULL_HANDLE;
		if (vkCreateFramebuffer(m_vk->device, &framebufferCreateInfo, nullptr, &framebuffer) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateFramebuffer failed for swapchain image.");
			destroySwapchain();
			return;
		}
		m_vk->swapchainFramebuffers.push_back(framebuffer);
	}

	m_vk->swapchainFormat = selectedFormat.format;
	m_vk->swapchainExtent = extent;
	m_vk->swapchainImageFences.assign(m_vk->swapchainImages.size(), VK_NULL_HANDLE);
	m_vk->frameSyncIndex = 0;
	m_vk->lastPresentedImageIndex = UINT32_MAX;
	resetFrameRenderData();

	LOG(LOG_VERBOSE, "Vulkan swapchain created: %ux%u", extent.width, extent.height);
#endif
}

void ContextImpl::destroySwapchain()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return;

	destroyDrawResources();

	if (!m_vk->swapchainFramebuffers.empty()) {
		for (VkFramebuffer framebuffer : m_vk->swapchainFramebuffers) {
			if (framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(m_vk->device, framebuffer, nullptr);
		}
		m_vk->swapchainFramebuffers.clear();
	}

	if (!m_vk->swapchainDepthImageViews.empty()) {
		for (VkImageView imageView : m_vk->swapchainDepthImageViews) {
			if (imageView != VK_NULL_HANDLE)
				vkDestroyImageView(m_vk->device, imageView, nullptr);
		}
		m_vk->swapchainDepthImageViews.clear();
	}

	if (!m_vk->swapchainDepthImages.empty()) {
		for (VkImage image : m_vk->swapchainDepthImages) {
			if (image != VK_NULL_HANDLE)
				vkDestroyImage(m_vk->device, image, nullptr);
		}
		m_vk->swapchainDepthImages.clear();
	}

	if (!m_vk->swapchainDepthMemories.empty()) {
		for (VkDeviceMemory memory : m_vk->swapchainDepthMemories) {
			if (memory != VK_NULL_HANDLE)
				vkFreeMemory(m_vk->device, memory, nullptr);
		}
		m_vk->swapchainDepthMemories.clear();
	}

	if (m_vk->renderPass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(m_vk->device, m_vk->renderPass, nullptr);
		m_vk->renderPass = VK_NULL_HANDLE;
	}

	if (!m_vk->swapchainImageViews.empty()) {
		for (VkImageView imageView : m_vk->swapchainImageViews) {
			if (imageView != VK_NULL_HANDLE)
				vkDestroyImageView(m_vk->device, imageView, nullptr);
		}
		m_vk->swapchainImageViews.clear();
	}

	m_vk->swapchainImages.clear();
	m_vk->swapchainImageFences.clear();
	m_vk->swapchainFormat = VK_FORMAT_UNDEFINED;
	m_vk->depthFormat = VK_FORMAT_UNDEFINED;
	m_vk->swapchainExtent = {};
	m_vk->lastPresentedImageIndex = UINT32_MAX;
	resetFrameRenderData();

	if (m_vk->swapchain == VK_NULL_HANDLE)
		return;
	vkDestroySwapchainKHR(m_vk->device, m_vk->swapchain, nullptr);
	m_vk->swapchain = VK_NULL_HANDLE;
#endif
}


} // namespace vulkan
