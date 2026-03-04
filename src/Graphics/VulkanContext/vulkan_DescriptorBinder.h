#pragma once

#include <array>
#include <cstdlib>
#include <vector>
#include <Types.h>
#include <Log.h>
#include "vulkan_DescriptorLayoutRegistry.h"
#include "vulkan_DrawRecorder.h"
#include "vulkan_Env.h"
#include "vulkan_TextureStore.h"

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
class DescriptorBinder
{
public:
	bool init(
		VkDevice _device,
		const DescriptorLayoutRegistry * _layoutRegistry,
		const TextureStore * _textureStore,
		u32 _framesInFlight)
	{
		destroy();
		if (_device == VK_NULL_HANDLE || _layoutRegistry == nullptr || _textureStore == nullptr || _framesInFlight == 0)
			return false;
		const VkDescriptorSetLayout setLayout = _layoutRegistry->getMainSetLayout();
		if (setLayout == VK_NULL_HANDLE)
			return false;

		m_device = _device;
		m_textureStore = _textureStore;
		m_frames.resize(_framesInFlight);
		if (!_createPool())
			return false;
		if (!_allocateSets(setLayout)) {
			destroy();
			return false;
		}
		m_currentFrameIndex = 0;
		return true;
	}

	void destroy()
	{
		if (m_device != VK_NULL_HANDLE) {
			if (m_descriptorPool != VK_NULL_HANDLE) {
				vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
				m_descriptorPool = VK_NULL_HANDLE;
			}
		}
		m_frames.clear();
		m_currentFrameIndex = 0;
		m_textureStore = nullptr;
		m_device = VK_NULL_HANDLE;
	}

	void beginFrame(u32 _frameIndex)
	{
		m_currentFrameIndex = _frameIndex;
	}

	bool bindPacket(VkCommandBuffer _commandBuffer, VkPipelineLayout _pipelineLayout, const DrawPacket & _packet) const
	{
		if (_commandBuffer == VK_NULL_HANDLE || _pipelineLayout == VK_NULL_HANDLE)
			return false;
		if (m_device == VK_NULL_HANDLE || m_textureStore == nullptr || m_frames.empty())
			return false;
		if (_packet.textureSlotMask == 0U)
			return false;

		const u32 frameIndex = m_currentFrameIndex % static_cast<u32>(m_frames.size());
		const VkDescriptorSet descriptorSet = m_frames[frameIndex].set;
		if (descriptorSet == VK_NULL_HANDLE)
			return false;

		static const bool debugBindings = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_BINDINGS", false);
		std::array<VkWriteDescriptorSet, DescriptorLayoutRegistry::kTextureBindingCount> writes{};
		std::array<VkDescriptorImageInfo, DescriptorLayoutRegistry::kTextureBindingCount> imageInfos{};
		u32 writeCount = 0;
		u32 resolvedMask = 0U;
		FrameDescriptors & frameDescriptors = const_cast<FrameDescriptors &>(m_frames[frameIndex]);

		for (u32 unit = 0; unit < DescriptorLayoutRegistry::kTextureBindingCount; ++unit) {
			if ((_packet.textureSlotMask & (1U << unit)) == 0U)
				continue;
			const TextureSlotReference & slotRef = _packet.textureSlots[unit];
			const TextureStore::TextureResource * texture = m_textureStore->getTexture(slotRef.texture);
			if (texture == nullptr) {
				if (debugBindings) {
					LOG(
						LOG_WARNING,
						"VK bindings debug: packet texture unit=%u handle=%u missing in TextureStore.",
						unit,
						static_cast<u32>(slotRef.texture));
				}
				continue;
			}
			if (texture->imageView == VK_NULL_HANDLE || texture->sampler == VK_NULL_HANDLE) {
				if (debugBindings) {
					LOG(
						LOG_WARNING,
						"VK bindings debug: packet texture unit=%u handle=%u unresolved resources imageView=%u sampler=%u initialized=%u size=%ux%u format=%u.",
						unit,
						static_cast<u32>(slotRef.texture),
						texture->imageView != VK_NULL_HANDLE ? 1U : 0U,
						texture->sampler != VK_NULL_HANDLE ? 1U : 0U,
						texture->initialized ? 1U : 0U,
						texture->width,
						texture->height,
						static_cast<u32>(texture->internalFormat));
				}
				continue;
			}
			resolvedMask |= (1U << unit);
			if ((frameDescriptors.textureMask & (1U << unit)) != 0U
				&& frameDescriptors.textures[unit] == slotRef.texture
				&& frameDescriptors.imageViews[unit] == texture->imageView
				&& frameDescriptors.samplers[unit] == texture->sampler) {
				continue;
			}
			if (writeCount >= DescriptorLayoutRegistry::kTextureBindingCount)
				break;

			VkDescriptorImageInfo & imageInfo = imageInfos[writeCount];
			imageInfo.sampler = texture->sampler;
			imageInfo.imageView = texture->imageView;
			imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				if (debugBindings) {
					static const u32 resolvedTextureLogLimit = []() -> u32 {
						const char * env = std::getenv("REALITYVK_VK_DEBUG_BINDINGS_RESOLVED_LIMIT");
						if (env == nullptr || env[0] == '\0')
							return 192U;
						return static_cast<u32>(std::strtoul(env, nullptr, 10));
					}();
					static u32 resolvedTextureLogCount = 0U;
					if (resolvedTextureLogCount < resolvedTextureLogLimit) {
					LOG(
						LOG_WARNING,
						"VK bindings debug: resolved texture unit=%u handle=%u vkFormat=%d internalFormat=%u size=%ux%u mipLevels=%u hasContent=%u",
						unit,
						static_cast<u32>(slotRef.texture),
						static_cast<int>(texture->vkFormat),
						static_cast<u32>(texture->internalFormat),
						texture->width,
						texture->height,
						texture->mipLevels,
						texture->hasContent ? 1U : 0U);
					++resolvedTextureLogCount;
				}
			}

			VkWriteDescriptorSet & write = writes[writeCount];
			write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			write.dstSet = descriptorSet;
			write.dstBinding = 0;
			write.dstArrayElement = unit;
			write.descriptorCount = 1;
			write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			write.pImageInfo = &imageInfo;
			++writeCount;
		}

		if (writeCount > 0)
			vkUpdateDescriptorSets(m_device, writeCount, writes.data(), 0, nullptr);
		if (debugBindings && _packet.textureSlotMask != 0U && resolvedMask == 0U) {
			LOG(
				LOG_WARNING,
				"VK bindings debug: packet textureSlotMask=0x%08x but resolvedMask=0x%08x (all texture descriptors unresolved).",
				_packet.textureSlotMask,
				resolvedMask);
		}
		frameDescriptors.textureMask = resolvedMask;
		for (u32 unit = 0; unit < DescriptorLayoutRegistry::kTextureBindingCount; ++unit) {
			if ((resolvedMask & (1U << unit)) != 0U) {
				const TextureStore::TextureResource * texture = m_textureStore->getTexture(_packet.textureSlots[unit].texture);
				frameDescriptors.textures[unit] = _packet.textureSlots[unit].texture;
				frameDescriptors.imageViews[unit] = texture != nullptr ? texture->imageView : VK_NULL_HANDLE;
				frameDescriptors.samplers[unit] = texture != nullptr ? texture->sampler : VK_NULL_HANDLE;
			} else {
				frameDescriptors.textures[unit] = graphics::ObjectHandle::null;
				frameDescriptors.imageViews[unit] = VK_NULL_HANDLE;
				frameDescriptors.samplers[unit] = VK_NULL_HANDLE;
			}
		}
		vkCmdBindDescriptorSets(
			_commandBuffer,
			VK_PIPELINE_BIND_POINT_GRAPHICS,
			_pipelineLayout,
			0,
			1,
			&descriptorSet,
			0,
			nullptr);
		return true;
	}

private:
	struct FrameDescriptors {
		VkDescriptorSet set = VK_NULL_HANDLE;
		u32 textureMask = 0U;
		std::array<graphics::ObjectHandle, DescriptorLayoutRegistry::kTextureBindingCount> textures{};
		std::array<VkImageView, DescriptorLayoutRegistry::kTextureBindingCount> imageViews{};
		std::array<VkSampler, DescriptorLayoutRegistry::kTextureBindingCount> samplers{};
	};

	bool _createPool()
	{
		const u32 descriptorCount = static_cast<u32>(m_frames.size()) * DescriptorLayoutRegistry::kTextureBindingCount;
		VkDescriptorPoolSize poolSize{};
		poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSize.descriptorCount = descriptorCount;

		VkDescriptorPoolCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		createInfo.maxSets = static_cast<u32>(m_frames.size());
		createInfo.poolSizeCount = 1;
		createInfo.pPoolSizes = &poolSize;
		return vkCreateDescriptorPool(m_device, &createInfo, nullptr, &m_descriptorPool) == VK_SUCCESS;
	}

	bool _allocateSets(VkDescriptorSetLayout _setLayout)
	{
		std::vector<VkDescriptorSetLayout> layouts(m_frames.size(), _setLayout);
		std::vector<VkDescriptorSet> sets(m_frames.size(), VK_NULL_HANDLE);
		VkDescriptorSetAllocateInfo allocateInfo{};
		allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocateInfo.descriptorPool = m_descriptorPool;
		allocateInfo.descriptorSetCount = static_cast<u32>(layouts.size());
		allocateInfo.pSetLayouts = layouts.data();
		if (vkAllocateDescriptorSets(m_device, &allocateInfo, sets.data()) != VK_SUCCESS)
			return false;
		for (size_t i = 0; i < m_frames.size(); ++i)
			m_frames[i].set = sets[i];
		return true;
	}

	VkDevice m_device = VK_NULL_HANDLE;
	const TextureStore * m_textureStore = nullptr;
	VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
	std::vector<FrameDescriptors> m_frames;
	u32 m_currentFrameIndex = 0;
};
#else
class DescriptorBinder
{
};
#endif

} // namespace vulkan
