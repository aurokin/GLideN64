#include "vulkan_ContextImpl_Internal.h"

#include <algorithm>
#include <cstring>
#include <vector>
#include <Log.h>
#ifdef MUPENPLUSAPI
#include <mupenplus/RealityVK_mupenplus.h>
#endif

namespace vulkan {

bool ContextImpl::initializeVulkanCore()
{
	if (!hasVulkanSupport())
		return false;
	if (!createInstance())
		return false;
	if (!createSurface()) {
		const bool hasPresentationTarget = m_presentationWindowInfo.system != graphics::Context::PresentationWindowInfo::WindowSystem::Unknown
			&& m_presentationWindowInfo.window != 0;
		if (hasPresentationTarget) {
			LOG(LOG_WARNING, "Vulkan presentation target was provided, but surface creation failed.");
		}
	}
	if (!selectPhysicalDevice())
		return false;
	if (!createDeviceAndQueue())
		return false;
	createSwapchain();
	m_coreReady = true;
	return true;
}

bool ContextImpl::createInstance()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk)
		m_vk.reset(new VulkanState);

	m_vk->surfaceExtensionEnabled = false;
	m_vk->xlibSurfaceExtensionEnabled = false;
	m_vk->win32SurfaceExtensionEnabled = false;
	m_vk->swapchainExtensionEnabled = false;

	u32 instanceExtensionCount = 0;
	std::vector<VkExtensionProperties> instanceExtensions;
	if (vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr) == VK_SUCCESS && instanceExtensionCount > 0U) {
		instanceExtensions.resize(instanceExtensionCount);
		if (vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, instanceExtensions.data()) != VK_SUCCESS) {
			instanceExtensions.clear();
		}
	}

	auto hasInstanceExtension = [&instanceExtensions](const char * _extensionName) {
		for (const auto & extension : instanceExtensions) {
			if (strcmp(extension.extensionName, _extensionName) == 0)
				return true;
		}
		return false;
	};

	std::vector<const char *> enabledInstanceExtensions;
	auto addEnabledInstanceExtension = [&enabledInstanceExtensions](const char * _extensionName) {
		for (const char * current : enabledInstanceExtensions) {
			if (strcmp(current, _extensionName) == 0)
				return;
		}
		enabledInstanceExtensions.push_back(_extensionName);
	};

	bool gotVidExtInstanceExtensions = false;
#ifdef MUPENPLUSAPI
	if (CoreVideo_VK_GetInstanceExtensions != nullptr) {
		const char ** vidExtExtensions = nullptr;
		uint32_t vidExtExtensionCount = 0;
		const m64p_error status = CoreVideo_VK_GetInstanceExtensions(&vidExtExtensions, &vidExtExtensionCount);
		if (status == M64ERR_SUCCESS && vidExtExtensions != nullptr && vidExtExtensionCount > 0U) {
			gotVidExtInstanceExtensions = true;
			for (uint32_t i = 0; i < vidExtExtensionCount; ++i) {
				const char * extensionName = vidExtExtensions[i];
				if (extensionName == nullptr || extensionName[0] == '\0')
					continue;
				if (!hasInstanceExtension(extensionName)) {
					LOG(LOG_WARNING, "Vulkan extension %s requested by core vidext is unavailable.", extensionName);
					return false;
				}
				addEnabledInstanceExtension(extensionName);
				if (strcmp(extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0)
					m_vk->surfaceExtensionEnabled = true;
#if defined(OS_LINUX)
				if (strcmp(extensionName, VK_KHR_XLIB_SURFACE_EXTENSION_NAME) == 0)
					m_vk->xlibSurfaceExtensionEnabled = true;
#endif
#if defined(OS_WINDOWS)
				if (strcmp(extensionName, VK_KHR_WIN32_SURFACE_EXTENSION_NAME) == 0)
					m_vk->win32SurfaceExtensionEnabled = true;
#endif
			}
		} else if (status != M64ERR_SUCCESS) {
			LOG(LOG_WARNING, "VidExt_VK_GetInstanceExtensions failed with error code: %d", status);
		}
	}
#endif

	if (!gotVidExtInstanceExtensions) {
		const bool hasPresentationTarget = m_presentationWindowInfo.system != graphics::Context::PresentationWindowInfo::WindowSystem::Unknown
			&& m_presentationWindowInfo.window != 0;
		if (hasPresentationTarget) {
			bool platformExtensionReady = false;

			switch (m_presentationWindowInfo.system) {
#if defined(OS_LINUX)
			case graphics::Context::PresentationWindowInfo::WindowSystem::Xlib:
				if (hasInstanceExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME)) {
					addEnabledInstanceExtension(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
					m_vk->xlibSurfaceExtensionEnabled = true;
					platformExtensionReady = true;
				} else {
					LOG(LOG_WARNING, "Vulkan extension %s is unavailable; Xlib surface creation is disabled.", VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
				}
				break;
#endif
#if defined(OS_WINDOWS)
			case graphics::Context::PresentationWindowInfo::WindowSystem::Win32:
				if (hasInstanceExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
					addEnabledInstanceExtension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
					m_vk->win32SurfaceExtensionEnabled = true;
					platformExtensionReady = true;
				} else {
					LOG(LOG_WARNING, "Vulkan extension %s is unavailable; Win32 surface creation is disabled.", VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
				}
				break;
#endif
			case graphics::Context::PresentationWindowInfo::WindowSystem::Unknown:
			default:
				break;
			}

			if (platformExtensionReady) {
				if (hasInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME)) {
					addEnabledInstanceExtension(VK_KHR_SURFACE_EXTENSION_NAME);
					m_vk->surfaceExtensionEnabled = true;
				} else {
					LOG(LOG_WARNING, "Vulkan extension %s is unavailable; surface creation is disabled.", VK_KHR_SURFACE_EXTENSION_NAME);
					m_vk->xlibSurfaceExtensionEnabled = false;
					m_vk->win32SurfaceExtensionEnabled = false;
					enabledInstanceExtensions.clear();
				}
			}
		}
	}

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "RealityVK Vulkan";
	appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
	appInfo.pEngineName = "RealityVK";
	appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
	appInfo.apiVersion = VK_API_VERSION_1_1;

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledExtensionCount = static_cast<u32>(enabledInstanceExtensions.size());
	createInfo.ppEnabledExtensionNames = enabledInstanceExtensions.empty() ? nullptr : enabledInstanceExtensions.data();

	const VkResult result = vkCreateInstance(&createInfo, nullptr, &m_vk->instance);
	if (result != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkCreateInstance failed: %d", static_cast<int>(result));
		return false;
	}
	return true;
#endif
}

bool ContextImpl::createSurface()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->instance == VK_NULL_HANDLE)
		return false;
	if (m_vk->surface != VK_NULL_HANDLE)
		return true;

#ifdef MUPENPLUSAPI
	if (CoreVideo_VK_GetSurface != nullptr) {
		void * surfaceHandle = nullptr;
		void * instanceHandle = reinterpret_cast<void *>(m_vk->instance);
		const m64p_error status = CoreVideo_VK_GetSurface(&surfaceHandle, instanceHandle);
		if (status == M64ERR_SUCCESS && surfaceHandle != nullptr) {
			m_vk->surface = reinterpret_cast<VkSurfaceKHR>(surfaceHandle);
			return true;
		}
		if (status != M64ERR_SUCCESS) {
			LOG(LOG_WARNING, "VidExt_VK_GetSurface failed with error code: %d", status);
		} else {
			LOG(LOG_WARNING, "VidExt_VK_GetSurface returned a null Vulkan surface.");
		}
	}
#endif

	switch (m_presentationWindowInfo.system) {
#if defined(OS_LINUX)
	case graphics::Context::PresentationWindowInfo::WindowSystem::Xlib:
		if (!m_vk->surfaceExtensionEnabled || !m_vk->xlibSurfaceExtensionEnabled)
			return false;
		if (m_presentationWindowInfo.display == nullptr || m_presentationWindowInfo.window == 0)
			return false;
	{
		VkXlibSurfaceCreateInfoKHR createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
		createInfo.dpy = reinterpret_cast<Display *>(m_presentationWindowInfo.display);
		createInfo.window = static_cast<Window>(m_presentationWindowInfo.window);

		const VkResult result = vkCreateXlibSurfaceKHR(m_vk->instance, &createInfo, nullptr, &m_vk->surface);
		if (result != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateXlibSurfaceKHR failed: %d", static_cast<int>(result));
			return false;
		}
		return true;
	}
#endif
#if defined(OS_WINDOWS)
	case graphics::Context::PresentationWindowInfo::WindowSystem::Win32:
		if (!m_vk->surfaceExtensionEnabled || !m_vk->win32SurfaceExtensionEnabled)
			return false;
		if (m_presentationWindowInfo.display == nullptr || m_presentationWindowInfo.window == 0)
			return false;
	{
		VkWin32SurfaceCreateInfoKHR createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
		createInfo.hinstance = reinterpret_cast<HINSTANCE>(m_presentationWindowInfo.display);
		createInfo.hwnd = reinterpret_cast<HWND>(m_presentationWindowInfo.window);

		const VkResult result = vkCreateWin32SurfaceKHR(m_vk->instance, &createInfo, nullptr, &m_vk->surface);
		if (result != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkCreateWin32SurfaceKHR failed: %d", static_cast<int>(result));
			return false;
		}
		return true;
	}
#else
	case graphics::Context::PresentationWindowInfo::WindowSystem::Win32:
		return false;
#endif
	case graphics::Context::PresentationWindowInfo::WindowSystem::Unknown:
	default:
		return false;
	}
#endif
}

bool ContextImpl::selectPhysicalDevice()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->instance == VK_NULL_HANDLE)
		return false;

	m_vk->physicalDevice = VK_NULL_HANDLE;
	m_vk->graphicsQueueFamily = UINT32_MAX;
	m_vk->presentQueueFamily = UINT32_MAX;

	u32 deviceCount = 0;
	if (vkEnumeratePhysicalDevices(m_vk->instance, &deviceCount, nullptr) != VK_SUCCESS || deviceCount == 0U) {
		LOG(LOG_WARNING, "Vulkan physical device enumeration failed or no device found.");
		return false;
	}

	std::vector<VkPhysicalDevice> devices(deviceCount, VK_NULL_HANDLE);
	if (vkEnumeratePhysicalDevices(m_vk->instance, &deviceCount, devices.data()) != VK_SUCCESS) {
		LOG(LOG_WARNING, "Vulkan physical device enumeration failed.");
		return false;
	}

	for (VkPhysicalDevice device : devices) {
		u32 queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
		if (queueFamilyCount == 0U)
			continue;

		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
		s32 graphicsQueueFamily = -1;
		s32 presentQueueFamily = -1;

		for (u32 i = 0; i < queueFamilyCount; ++i) {
			if ((queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && graphicsQueueFamily < 0) {
				graphicsQueueFamily = static_cast<s32>(i);
			}

			if (m_vk->surface != VK_NULL_HANDLE && presentQueueFamily < 0) {
				VkBool32 presentSupported = VK_FALSE;
				if (vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_vk->surface, &presentSupported) == VK_SUCCESS && presentSupported == VK_TRUE) {
					presentQueueFamily = static_cast<s32>(i);
				}
			}
		}

		if (graphicsQueueFamily < 0)
			continue;
		if (m_vk->surface != VK_NULL_HANDLE && presentQueueFamily < 0)
			continue;

		if (m_vk->surface == VK_NULL_HANDLE)
			presentQueueFamily = graphicsQueueFamily;

		m_vk->physicalDevice = device;
		m_vk->graphicsQueueFamily = static_cast<u32>(graphicsQueueFamily);
		m_vk->presentQueueFamily = static_cast<u32>(presentQueueFamily);

		VkPhysicalDeviceProperties properties{};
		vkGetPhysicalDeviceProperties(device, &properties);
		VkPhysicalDeviceFeatures features{};
		vkGetPhysicalDeviceFeatures(device, &features);
		m_maxTextureSize = static_cast<s32>(std::max<u32>(1U, properties.limits.maxImageDimension2D));
		m_maxLineWidth = features.wideLines == VK_TRUE
			? std::max(1.0f, properties.limits.lineWidthRange[1])
			: 1.0f;
		m_maxAnisotropy = std::max(1.0f, properties.limits.maxSamplerAnisotropy);

		const VkSampleCountFlags sampleCounts = properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts;
		if (sampleCounts & VK_SAMPLE_COUNT_16_BIT) {
			m_maxMsaaLevel = 16;
		} else if (sampleCounts & VK_SAMPLE_COUNT_8_BIT) {
			m_maxMsaaLevel = 8;
		} else if (sampleCounts & VK_SAMPLE_COUNT_4_BIT) {
			m_maxMsaaLevel = 4;
		} else if (sampleCounts & VK_SAMPLE_COUNT_2_BIT) {
			m_maxMsaaLevel = 2;
		} else {
			m_maxMsaaLevel = 1;
		}
		return true;
	}

	if (m_vk->surface != VK_NULL_HANDLE) {
		LOG(LOG_WARNING, "No Vulkan queue family with presentation support was found.");
	} else {
		LOG(LOG_WARNING, "No Vulkan queue family with graphics capability was found.");
	}
	return false;
#endif
}

bool ContextImpl::createDeviceAndQueue()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->physicalDevice == VK_NULL_HANDLE || m_vk->graphicsQueueFamily == UINT32_MAX)
		return false;
	if (m_vk->presentQueueFamily == UINT32_MAX)
		m_vk->presentQueueFamily = m_vk->graphicsQueueFamily;

	m_vk->swapchainExtensionEnabled = false;
	m_vk->graphicsQueue = VK_NULL_HANDLE;
	m_vk->presentQueue = VK_NULL_HANDLE;

	const float queuePriority = 1.0f;
	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	queueCreateInfos.reserve(2);

	VkDeviceQueueCreateInfo graphicsQueueCreateInfo{};
	graphicsQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	graphicsQueueCreateInfo.queueFamilyIndex = m_vk->graphicsQueueFamily;
	graphicsQueueCreateInfo.queueCount = 1;
	graphicsQueueCreateInfo.pQueuePriorities = &queuePriority;
	queueCreateInfos.push_back(graphicsQueueCreateInfo);

	if (m_vk->presentQueueFamily != m_vk->graphicsQueueFamily) {
		VkDeviceQueueCreateInfo presentQueueCreateInfo{};
		presentQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		presentQueueCreateInfo.queueFamilyIndex = m_vk->presentQueueFamily;
		presentQueueCreateInfo.queueCount = 1;
		presentQueueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(presentQueueCreateInfo);
	}

	VkPhysicalDeviceFeatures availableFeatures{};
	vkGetPhysicalDeviceFeatures(m_vk->physicalDevice, &availableFeatures);
	VkPhysicalDeviceFeatures requestedFeatures{};
	if (availableFeatures.samplerAnisotropy == VK_TRUE)
		requestedFeatures.samplerAnisotropy = VK_TRUE;
	if (availableFeatures.wideLines == VK_TRUE)
		requestedFeatures.wideLines = VK_TRUE;
	if (availableFeatures.dualSrcBlend == VK_TRUE)
		requestedFeatures.dualSrcBlend = VK_TRUE;

	u32 extensionCount = 0;
	std::vector<const char *> extensions;
	if (vkEnumerateDeviceExtensionProperties(m_vk->physicalDevice, nullptr, &extensionCount, nullptr) == VK_SUCCESS && extensionCount > 0U) {
		std::vector<VkExtensionProperties> extensionProperties(extensionCount);
		if (vkEnumerateDeviceExtensionProperties(m_vk->physicalDevice, nullptr, &extensionCount, extensionProperties.data()) == VK_SUCCESS) {
			for (const auto & extension : extensionProperties) {
				if (strcmp(extension.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
					if (m_vk->surface != VK_NULL_HANDLE) {
						extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
						m_vk->swapchainExtensionEnabled = true;
					}
					break;
				}
			}
		}
	}

	if (m_vk->surface != VK_NULL_HANDLE && !m_vk->swapchainExtensionEnabled)
		LOG(LOG_WARNING, "Vulkan device extension %s is unavailable; swapchain creation is disabled.", VK_KHR_SWAPCHAIN_EXTENSION_NAME);

	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = static_cast<u32>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();
	createInfo.pEnabledFeatures = &requestedFeatures;
	createInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();

	const VkResult result = vkCreateDevice(m_vk->physicalDevice, &createInfo, nullptr, &m_vk->device);
	if (result != VK_SUCCESS) {
		LOG(LOG_WARNING, "vkCreateDevice failed: %d", static_cast<int>(result));
		return false;
	}

	vkGetDeviceQueue(m_vk->device, m_vk->graphicsQueueFamily, 0, &m_vk->graphicsQueue);
	vkGetDeviceQueue(m_vk->device, m_vk->presentQueueFamily, 0, &m_vk->presentQueue);
	m_vk->wideLinesEnabled = requestedFeatures.wideLines == VK_TRUE;
	m_vk->dualSrcBlendEnabled = requestedFeatures.dualSrcBlend == VK_TRUE;
	if (!m_vk->wideLinesEnabled)
		m_maxLineWidth = 1.0f;
	if (!m_vk->dualSrcBlendEnabled)
		LOG(LOG_WARNING, "Vulkan dual-source blending unavailable; dual-source path stays disabled.");
	if (!createPresentSyncObjects()) {
		LOG(LOG_WARNING, "Failed to create Vulkan presentation sync primitives.");
		vkDestroyDevice(m_vk->device, nullptr);
		m_vk->device = VK_NULL_HANDLE;
		m_vk->graphicsQueue = VK_NULL_HANDLE;
		m_vk->presentQueue = VK_NULL_HANDLE;
		return false;
	}
	return true;
#endif
}

bool ContextImpl::createPresentSyncObjects()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return false;

	destroyPresentSyncObjects();

	constexpr size_t kFramesInFlight = 2;
	VkSemaphoreCreateInfo semaphoreCreateInfo{};
	semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	VkFenceCreateInfo fenceCreateInfo{};
	fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	VkCommandPoolCreateInfo commandPoolCreateInfo{};
	commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	commandPoolCreateInfo.queueFamilyIndex = m_vk->graphicsQueueFamily;

	m_vk->frameSync.clear();
	m_vk->frameSync.resize(kFramesInFlight);
	for (VulkanState::FrameSync & frame : m_vk->frameSync) {
		if (vkCreateSemaphore(m_vk->device, &semaphoreCreateInfo, nullptr, &frame.imageAvailable) != VK_SUCCESS) {
			destroyPresentSyncObjects();
			return false;
		}
		if (vkCreateSemaphore(m_vk->device, &semaphoreCreateInfo, nullptr, &frame.renderFinished) != VK_SUCCESS) {
			destroyPresentSyncObjects();
			return false;
		}
		if (vkCreateFence(m_vk->device, &fenceCreateInfo, nullptr, &frame.inFlight) != VK_SUCCESS) {
			destroyPresentSyncObjects();
			return false;
		}
	}

	if (vkCreateCommandPool(m_vk->device, &commandPoolCreateInfo, nullptr, &m_vk->commandPool) != VK_SUCCESS) {
		destroyPresentSyncObjects();
		return false;
	}

	m_vk->frameCommandBuffers.clear();
	m_vk->frameCommandBuffers.resize(kFramesInFlight, VK_NULL_HANDLE);
	VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
	commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	commandBufferAllocateInfo.commandPool = m_vk->commandPool;
	commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	commandBufferAllocateInfo.commandBufferCount = static_cast<u32>(m_vk->frameCommandBuffers.size());
	if (vkAllocateCommandBuffers(m_vk->device, &commandBufferAllocateInfo, m_vk->frameCommandBuffers.data()) != VK_SUCCESS) {
		destroyPresentSyncObjects();
		return false;
	}
	if (!m_vk->uploadArena.init(
		m_vk->physicalDevice,
		m_vk->device,
		static_cast<u32>(kFramesInFlight),
		kInitialFrameVertexBufferSize)) {
		destroyPresentSyncObjects();
		return false;
	}
	if (!m_vk->textureStore.init(
		m_vk->physicalDevice,
		m_vk->device,
		m_vk->commandPool,
		m_vk->graphicsQueue,
		m_maxAnisotropy)) {
		destroyPresentSyncObjects();
		return false;
	}

	m_vk->frameSyncIndex = 0;
	m_vk->drawRecorder.markAllStateDirty();
	return true;
#endif
}


void ContextImpl::destroyPresentSyncObjects()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->device == VK_NULL_HANDLE)
		return;

	m_vk->bindingState.clear();
	m_vk->framebufferStore.clear();
	m_vk->textureStore.destroy();
	m_vk->uploadArena.destroy();

	for (VulkanState::FrameSync & frame : m_vk->frameSync) {
		if (frame.imageAvailable != VK_NULL_HANDLE) {
			vkDestroySemaphore(m_vk->device, frame.imageAvailable, nullptr);
			frame.imageAvailable = VK_NULL_HANDLE;
		}
		if (frame.renderFinished != VK_NULL_HANDLE) {
			vkDestroySemaphore(m_vk->device, frame.renderFinished, nullptr);
			frame.renderFinished = VK_NULL_HANDLE;
		}
		if (frame.inFlight != VK_NULL_HANDLE) {
			vkDestroyFence(m_vk->device, frame.inFlight, nullptr);
			frame.inFlight = VK_NULL_HANDLE;
		}
	}

	m_vk->frameCommandBuffers.clear();
	if (m_vk->commandPool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(m_vk->device, m_vk->commandPool, nullptr);
		m_vk->commandPool = VK_NULL_HANDLE;
	}

	m_vk->frameSync.clear();
	m_vk->frameSyncIndex = 0;
#endif
}

void ContextImpl::destroySurface()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk || m_vk->instance == VK_NULL_HANDLE || m_vk->surface == VK_NULL_HANDLE)
		return;
	vkDestroySurfaceKHR(m_vk->instance, m_vk->surface, nullptr);
	m_vk->surface = VK_NULL_HANDLE;
#endif
}

void ContextImpl::shutdownVulkanCore()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk) {
		m_coreReady = false;
		return;
	}

	if (m_vk->device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_vk->device);

	destroySwapchain();
	destroyPresentSyncObjects();

	if (m_vk->device != VK_NULL_HANDLE) {
		vkDestroyDevice(m_vk->device, nullptr);
		m_vk->device = VK_NULL_HANDLE;
	}

	destroySurface();

	if (m_vk->instance != VK_NULL_HANDLE) {
		vkDestroyInstance(m_vk->instance, nullptr);
		m_vk->instance = VK_NULL_HANDLE;
	}
#endif

	m_vk.reset();
	m_coreReady = false;
}


} // namespace vulkan
