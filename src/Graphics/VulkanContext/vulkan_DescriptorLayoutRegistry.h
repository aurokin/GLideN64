#pragma once

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

#include "vulkan_BindingLimits.h"
#include "vulkan_DrawShaderConfig.h"

namespace vulkan {

#if REALITYVK_INTERNAL_HAS_VULKAN_HEADERS
class DescriptorLayoutRegistry
{
public:
	static constexpr u32 kTextureBindingCount = binding_limits::kTextureUnits;

	bool init(VkDevice _device)
	{
		destroy();
		if (_device == VK_NULL_HANDLE)
			return false;
		m_device = _device;
		return true;
	}

	void destroy()
	{
		if (m_device != VK_NULL_HANDLE) {
			if (m_basicColorPipelineLayout != VK_NULL_HANDLE) {
				vkDestroyPipelineLayout(m_device, m_basicColorPipelineLayout, nullptr);
				m_basicColorPipelineLayout = VK_NULL_HANDLE;
			}
			if (m_mainSetLayout != VK_NULL_HANDLE) {
				vkDestroyDescriptorSetLayout(m_device, m_mainSetLayout, nullptr);
				m_mainSetLayout = VK_NULL_HANDLE;
			}
		}
		m_device = VK_NULL_HANDLE;
	}

	bool ensureBasicColorPipelineLayout()
	{
		if (m_device == VK_NULL_HANDLE)
			return false;
		if (m_basicColorPipelineLayout != VK_NULL_HANDLE)
			return true;
		if (!_ensureMainSetLayout())
			return false;

		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
		pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutCreateInfo.setLayoutCount = 1;
		pipelineLayoutCreateInfo.pSetLayouts = &m_mainSetLayout;
		VkPushConstantRange pushConstantRange{};
		pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		pushConstantRange.offset = 0;
		pushConstantRange.size = static_cast<u32>(sizeof(DrawPushConstants));
		pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
		pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;
		return vkCreatePipelineLayout(
			m_device,
			&pipelineLayoutCreateInfo,
			nullptr,
			&m_basicColorPipelineLayout) == VK_SUCCESS;
	}

	VkPipelineLayout getBasicColorPipelineLayout() const
	{
		return m_basicColorPipelineLayout;
	}

	VkDescriptorSetLayout getMainSetLayout() const
	{
		return m_mainSetLayout;
	}

private:
	bool _ensureMainSetLayout()
	{
		if (m_device == VK_NULL_HANDLE)
			return false;
		if (m_mainSetLayout != VK_NULL_HANDLE)
			return true;
		VkDescriptorSetLayoutBinding textureBinding{};
		textureBinding.binding = 0;
		textureBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		textureBinding.descriptorCount = kTextureBindingCount;
		textureBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		textureBinding.pImmutableSamplers = nullptr;
		VkDescriptorSetLayoutCreateInfo setLayoutCreateInfo{};
		setLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		setLayoutCreateInfo.bindingCount = 1;
		setLayoutCreateInfo.pBindings = &textureBinding;
		return vkCreateDescriptorSetLayout(m_device, &setLayoutCreateInfo, nullptr, &m_mainSetLayout) == VK_SUCCESS;
	}

	VkDevice m_device = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_mainSetLayout = VK_NULL_HANDLE;
	VkPipelineLayout m_basicColorPipelineLayout = VK_NULL_HANDLE;
};
#else
class DescriptorLayoutRegistry
{
};
#endif

} // namespace vulkan
