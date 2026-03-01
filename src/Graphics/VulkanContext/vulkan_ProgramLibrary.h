#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <vector>
#include <Graphics/CombinerProgram.h>
#include "CombinerKey.h"

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

#include "vulkan_DescriptorLayoutRegistry.h"

namespace vulkan {

#if REALITYVK_INTERNAL_HAS_VULKAN_HEADERS
class ProgramLibrary
{
public:
	bool init(VkDevice _device, DescriptorLayoutRegistry * _layoutRegistry)
	{
		destroy();
		if (_device == VK_NULL_HANDLE || _layoutRegistry == nullptr)
			return false;
		m_device = _device;
		m_layoutRegistry = _layoutRegistry;
		return true;
	}

	void destroy()
	{
		if (m_device != VK_NULL_HANDLE) {
			if (m_basicColorVertexShader != VK_NULL_HANDLE) {
				vkDestroyShaderModule(m_device, m_basicColorVertexShader, nullptr);
				m_basicColorVertexShader = VK_NULL_HANDLE;
			}
			if (m_basicColorFragmentShader != VK_NULL_HANDLE) {
				vkDestroyShaderModule(m_device, m_basicColorFragmentShader, nullptr);
				m_basicColorFragmentShader = VK_NULL_HANDLE;
			}
			if (m_basicTexturedVertexShader != VK_NULL_HANDLE) {
				vkDestroyShaderModule(m_device, m_basicTexturedVertexShader, nullptr);
				m_basicTexturedVertexShader = VK_NULL_HANDLE;
			}
			if (m_basicTexturedFragmentShader != VK_NULL_HANDLE) {
				vkDestroyShaderModule(m_device, m_basicTexturedFragmentShader, nullptr);
				m_basicTexturedFragmentShader = VK_NULL_HANDLE;
			}
		}
		m_layoutRegistry = nullptr;
		m_device = VK_NULL_HANDLE;
	}

	bool loadBasicColorProgram(
		const unsigned char * _vertexBytes,
		size_t _vertexSize,
		const unsigned char * _fragmentBytes,
		size_t _fragmentSize)
	{
		if (m_device == VK_NULL_HANDLE || m_layoutRegistry == nullptr)
			return false;
		if (_vertexBytes == nullptr || _fragmentBytes == nullptr || _vertexSize == 0 || _fragmentSize == 0)
			return false;
		if (!m_layoutRegistry->ensureBasicColorPipelineLayout())
			return false;

		_destroyBasicColorShaders();
		if (!_createShaderModule(_vertexBytes, _vertexSize, m_basicColorVertexShader))
			return false;
		if (!_createShaderModule(_fragmentBytes, _fragmentSize, m_basicColorFragmentShader)) {
			_destroyBasicColorShaders();
			return false;
		}
		return true;
	}

	bool loadBasicTexturedProgram(
		const unsigned char * _vertexBytes,
		size_t _vertexSize,
		const unsigned char * _fragmentBytes,
		size_t _fragmentSize)
	{
		if (m_device == VK_NULL_HANDLE || m_layoutRegistry == nullptr)
			return false;
		if (_vertexBytes == nullptr || _fragmentBytes == nullptr || _vertexSize == 0 || _fragmentSize == 0)
			return false;
		if (!m_layoutRegistry->ensureBasicColorPipelineLayout())
			return false;

		_destroyBasicTexturedShaders();
		if (!_createShaderModule(_vertexBytes, _vertexSize, m_basicTexturedVertexShader))
			return false;
		if (!_createShaderModule(_fragmentBytes, _fragmentSize, m_basicTexturedFragmentShader)) {
			_destroyBasicTexturedShaders();
			return false;
		}
		return true;
	}

	VkShaderModule getBasicColorVertexShader() const
	{
		return m_basicColorVertexShader;
	}

	VkShaderModule getBasicColorFragmentShader() const
	{
		return m_basicColorFragmentShader;
	}

	VkShaderModule getBasicTexturedVertexShader() const
	{
		return m_basicTexturedVertexShader;
	}

	VkShaderModule getBasicTexturedFragmentShader() const
	{
		return m_basicTexturedFragmentShader;
	}

	VkPipelineLayout getBasicColorPipelineLayout() const
	{
		if (m_layoutRegistry == nullptr)
			return VK_NULL_HANDLE;
		return m_layoutRegistry->getBasicColorPipelineLayout();
	}

	graphics::CombinerProgram * createCombinerProgram(
		const CombinerKey & _key,
		const std::function<graphics::CombinerProgram * (const CombinerKey &)> & _factory) const
	{
		if (!_factory)
			return nullptr;
		return _factory(_key);
	}

private:
	void _destroyBasicColorShaders()
	{
		if (m_device == VK_NULL_HANDLE)
			return;
		if (m_basicColorVertexShader != VK_NULL_HANDLE) {
			vkDestroyShaderModule(m_device, m_basicColorVertexShader, nullptr);
			m_basicColorVertexShader = VK_NULL_HANDLE;
		}
		if (m_basicColorFragmentShader != VK_NULL_HANDLE) {
			vkDestroyShaderModule(m_device, m_basicColorFragmentShader, nullptr);
			m_basicColorFragmentShader = VK_NULL_HANDLE;
		}
	}

	void _destroyBasicTexturedShaders()
	{
		if (m_device == VK_NULL_HANDLE)
			return;
		if (m_basicTexturedVertexShader != VK_NULL_HANDLE) {
			vkDestroyShaderModule(m_device, m_basicTexturedVertexShader, nullptr);
			m_basicTexturedVertexShader = VK_NULL_HANDLE;
		}
		if (m_basicTexturedFragmentShader != VK_NULL_HANDLE) {
			vkDestroyShaderModule(m_device, m_basicTexturedFragmentShader, nullptr);
			m_basicTexturedFragmentShader = VK_NULL_HANDLE;
		}
	}

	bool _createShaderModule(const unsigned char * _bytes, size_t _size, VkShaderModule & _outShader) const
	{
		if (m_device == VK_NULL_HANDLE || _bytes == nullptr || _size == 0)
			return false;
		std::vector<u32> alignedCode((_size + sizeof(u32) - 1U) / sizeof(u32), 0U);
		std::memcpy(alignedCode.data(), _bytes, _size);

		VkShaderModuleCreateInfo createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		createInfo.codeSize = _size;
		createInfo.pCode = alignedCode.data();
		return vkCreateShaderModule(m_device, &createInfo, nullptr, &_outShader) == VK_SUCCESS;
	}

	VkDevice m_device = VK_NULL_HANDLE;
	DescriptorLayoutRegistry * m_layoutRegistry = nullptr;
	VkShaderModule m_basicColorVertexShader = VK_NULL_HANDLE;
	VkShaderModule m_basicColorFragmentShader = VK_NULL_HANDLE;
	VkShaderModule m_basicTexturedVertexShader = VK_NULL_HANDLE;
	VkShaderModule m_basicTexturedFragmentShader = VK_NULL_HANDLE;
};
#else
class ProgramLibrary
{
};
#endif

} // namespace vulkan
