#pragma once

#include <algorithm>
#include <vector>
#include <Types.h>

#ifndef REALITYVK_INTERNAL_HAS_VULKAN_HEADERS
#if __has_include(<vulkan/vulkan.h>)
#define REALITYVK_INTERNAL_HAS_VULKAN_HEADERS 1
#include <vulkan/vulkan.h>
#else
#define REALITYVK_INTERNAL_HAS_VULKAN_HEADERS 0
#endif
#elif REALITYVK_INTERNAL_HAS_VULKAN_HEADERS
#include <vulkan/vulkan.h>
#endif

namespace vulkan {

#if REALITYVK_INTERNAL_HAS_VULKAN_HEADERS
class UploadArena
{
public:
	struct Allocation {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceSize offset = 0;
		void * mapped = nullptr;
	};

	bool init(VkPhysicalDevice _physicalDevice, VkDevice _device, u32 _frameCount, VkDeviceSize _initialFrameCapacity)
	{
		destroy();
		if (_physicalDevice == VK_NULL_HANDLE || _device == VK_NULL_HANDLE || _frameCount == 0)
			return false;

		m_physicalDevice = _physicalDevice;
		m_device = _device;
		m_initialFrameCapacity = std::max<VkDeviceSize>(_initialFrameCapacity, 4096U);
		m_frames.resize(_frameCount);
		for (FrameArena & frame : m_frames) {
			if (!_createFrameArena(frame, m_initialFrameCapacity)) {
				destroy();
				return false;
			}
		}
		return true;
	}

	void destroy()
	{
		if (m_device != VK_NULL_HANDLE) {
			for (FrameArena & frame : m_frames) {
				if (frame.mapped != nullptr) {
					vkUnmapMemory(m_device, frame.memory);
					frame.mapped = nullptr;
				}
				if (frame.buffer != VK_NULL_HANDLE) {
					vkDestroyBuffer(m_device, frame.buffer, nullptr);
					frame.buffer = VK_NULL_HANDLE;
				}
				if (frame.memory != VK_NULL_HANDLE) {
					vkFreeMemory(m_device, frame.memory, nullptr);
					frame.memory = VK_NULL_HANDLE;
				}
				frame.capacity = 0;
				frame.writeOffset = 0;
			}
		}
		m_frames.clear();
		m_physicalDevice = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
		m_initialFrameCapacity = 0;
	}

	void beginFrame(u32 _frameIndex)
	{
		if (_frameIndex >= m_frames.size())
			return;
		m_frames[_frameIndex].writeOffset = 0;
	}

	bool allocate(u32 _frameIndex, VkDeviceSize _size, VkDeviceSize _alignment, Allocation & _allocation)
	{
		if (_frameIndex >= m_frames.size() || _size == 0)
			return false;
		FrameArena & frame = m_frames[_frameIndex];
		if (frame.buffer == VK_NULL_HANDLE || frame.memory == VK_NULL_HANDLE || frame.mapped == nullptr)
			return false;

		const VkDeviceSize alignedOffset = _alignUp(frame.writeOffset, std::max<VkDeviceSize>(1, _alignment));
		const VkDeviceSize requiredSize = alignedOffset + _size;
		if (requiredSize > frame.capacity) {
			const VkDeviceSize grownCapacity = std::max(requiredSize, std::max<VkDeviceSize>(frame.capacity * 2U, m_initialFrameCapacity));
			if (!_recreateFrameArena(frame, grownCapacity))
				return false;
		}

		const VkDeviceSize finalOffset = _alignUp(frame.writeOffset, std::max<VkDeviceSize>(1, _alignment));
		_allocation.buffer = frame.buffer;
		_allocation.offset = finalOffset;
		_allocation.mapped = frame.mapped + finalOffset;
		frame.writeOffset = finalOffset + _size;
		return true;
	}

private:
	struct FrameArena {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		u8 * mapped = nullptr;
		VkDeviceSize capacity = 0;
		VkDeviceSize writeOffset = 0;
	};

	static VkDeviceSize _alignUp(VkDeviceSize _value, VkDeviceSize _alignment)
	{
		return (_value + _alignment - 1U) & ~(_alignment - 1U);
	}

	u32 _findMemoryType(u32 _typeFilter, VkMemoryPropertyFlags _properties) const
	{
		VkPhysicalDeviceMemoryProperties memoryProperties{};
		vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
		for (u32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
			if ((_typeFilter & (1U << i)) == 0U)
				continue;
			if ((memoryProperties.memoryTypes[i].propertyFlags & _properties) == _properties)
				return i;
		}
		return UINT32_MAX;
	}

	bool _createFrameArena(FrameArena & _frame, VkDeviceSize _capacity)
	{
		VkBufferCreateInfo bufferCreateInfo{};
		bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferCreateInfo.size = _capacity;
		bufferCreateInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(m_device, &bufferCreateInfo, nullptr, &_frame.buffer) != VK_SUCCESS)
			return false;

		VkMemoryRequirements memoryRequirements{};
		vkGetBufferMemoryRequirements(m_device, _frame.buffer, &memoryRequirements);
		const u32 memoryTypeIndex = _findMemoryType(
			memoryRequirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (memoryTypeIndex == UINT32_MAX) {
			vkDestroyBuffer(m_device, _frame.buffer, nullptr);
			_frame.buffer = VK_NULL_HANDLE;
			return false;
		}

		VkMemoryAllocateInfo allocateInfo{};
		allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocateInfo.allocationSize = memoryRequirements.size;
		allocateInfo.memoryTypeIndex = memoryTypeIndex;
		if (vkAllocateMemory(m_device, &allocateInfo, nullptr, &_frame.memory) != VK_SUCCESS) {
			vkDestroyBuffer(m_device, _frame.buffer, nullptr);
			_frame.buffer = VK_NULL_HANDLE;
			return false;
		}

		if (vkBindBufferMemory(m_device, _frame.buffer, _frame.memory, 0) != VK_SUCCESS) {
			vkDestroyBuffer(m_device, _frame.buffer, nullptr);
			vkFreeMemory(m_device, _frame.memory, nullptr);
			_frame.buffer = VK_NULL_HANDLE;
			_frame.memory = VK_NULL_HANDLE;
			return false;
		}

		void * mapped = nullptr;
		if (vkMapMemory(m_device, _frame.memory, 0, memoryRequirements.size, 0, &mapped) != VK_SUCCESS || mapped == nullptr) {
			vkDestroyBuffer(m_device, _frame.buffer, nullptr);
			vkFreeMemory(m_device, _frame.memory, nullptr);
			_frame.buffer = VK_NULL_HANDLE;
			_frame.memory = VK_NULL_HANDLE;
			return false;
		}

		_frame.mapped = static_cast<u8 *>(mapped);
		_frame.capacity = _capacity;
		_frame.writeOffset = 0;
		return true;
	}

	bool _recreateFrameArena(FrameArena & _frame, VkDeviceSize _newCapacity)
	{
		if (_frame.mapped != nullptr) {
			vkUnmapMemory(m_device, _frame.memory);
			_frame.mapped = nullptr;
		}
		if (_frame.buffer != VK_NULL_HANDLE) {
			vkDestroyBuffer(m_device, _frame.buffer, nullptr);
			_frame.buffer = VK_NULL_HANDLE;
		}
		if (_frame.memory != VK_NULL_HANDLE) {
			vkFreeMemory(m_device, _frame.memory, nullptr);
			_frame.memory = VK_NULL_HANDLE;
		}
		_frame.capacity = 0;
		_frame.writeOffset = 0;
		return _createFrameArena(_frame, _newCapacity);
	}

	VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
	VkDevice m_device = VK_NULL_HANDLE;
	VkDeviceSize m_initialFrameCapacity = 0;
	std::vector<FrameArena> m_frames;
};
#else
class UploadArena
{
};
#endif

} // namespace vulkan
