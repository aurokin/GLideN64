#pragma once

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <unordered_map>
#include <functional>
#include "vulkan_DrawRecorder.h"
#include "vulkan_Env.h"

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
class PipelineCache
{
public:
	bool init(
		VkDevice _device,
		VkRenderPass _renderPass,
		VkPipelineLayout _layout,
		VkShaderModule _colorVertexShader,
		VkShaderModule _colorFragmentShader,
		VkShaderModule _texturedVertexShader,
		VkShaderModule _texturedFragmentShader)
	{
		destroy();
		if (_device == VK_NULL_HANDLE
			|| _renderPass == VK_NULL_HANDLE
			|| _layout == VK_NULL_HANDLE
			|| _colorVertexShader == VK_NULL_HANDLE
			|| _colorFragmentShader == VK_NULL_HANDLE) {
			return false;
		}
		m_device = _device;
		m_renderPass = _renderPass;
		m_layout = _layout;
		m_colorVertexShader = _colorVertexShader;
		m_colorFragmentShader = _colorFragmentShader;
		m_texturedVertexShader = _texturedVertexShader;
		m_texturedFragmentShader = _texturedFragmentShader;
		return true;
	}

	void destroy()
	{
		if (m_device != VK_NULL_HANDLE) {
			for (auto & item : m_pipelines) {
				if (item.second != VK_NULL_HANDLE)
					vkDestroyPipeline(m_device, item.second, nullptr);
			}
		}
		m_pipelines.clear();
		m_device = VK_NULL_HANDLE;
		m_renderPass = VK_NULL_HANDLE;
		m_layout = VK_NULL_HANDLE;
		m_colorVertexShader = VK_NULL_HANDLE;
		m_colorFragmentShader = VK_NULL_HANDLE;
		m_texturedVertexShader = VK_NULL_HANDLE;
		m_texturedFragmentShader = VK_NULL_HANDLE;
	}

	VkPipeline getOrCreate(const DrawPacket & _packet)
	{
		if (m_device == VK_NULL_HANDLE || m_renderPass == VK_NULL_HANDLE || m_layout == VK_NULL_HANDLE)
			return VK_NULL_HANDLE;
		const PipelineKey key = _buildKey(_packet);
		const auto cached = m_pipelines.find(key);
		if (cached != m_pipelines.end())
			return cached->second;

		VkPipeline pipeline = _createPipeline(key);
		if (pipeline == VK_NULL_HANDLE)
			return VK_NULL_HANDLE;
		m_pipelines.emplace(key, pipeline);
		return pipeline;
	}

private:
	struct PipelineKey {
		PrimitiveType primitive = PrimitiveType::Triangles;
		bool blendEnabled = false;
		BlendFactor srcColor = BlendFactor::One;
		BlendFactor dstColor = BlendFactor::Zero;
		BlendFactor srcAlpha = BlendFactor::One;
		BlendFactor dstAlpha = BlendFactor::Zero;
		bool depthTestEnabled = false;
		bool depthWriteEnabled = false;
		CompareMode depthCompare = CompareMode::kLessOrEqual;
		CullMode cullMode = CullMode::kNone;
		bool textured = false;

		bool operator==(const PipelineKey & _other) const
		{
			return primitive == _other.primitive
				&& blendEnabled == _other.blendEnabled
				&& srcColor == _other.srcColor
				&& dstColor == _other.dstColor
				&& srcAlpha == _other.srcAlpha
				&& dstAlpha == _other.dstAlpha
				&& depthTestEnabled == _other.depthTestEnabled
				&& depthWriteEnabled == _other.depthWriteEnabled
				&& depthCompare == _other.depthCompare
				&& cullMode == _other.cullMode
				&& textured == _other.textured;
		}
	};

	struct PipelineKeyHash {
		size_t operator()(const PipelineKey & _key) const
		{
			size_t hashValue = 0;
			auto hashCombine = [&hashValue](size_t _value) {
				hashValue ^= _value + 0x9e3779b9 + (hashValue << 6) + (hashValue >> 2);
			};
			hashCombine(std::hash<u32>()(static_cast<u32>(_key.primitive)));
			hashCombine(std::hash<u32>()(_key.blendEnabled ? 1U : 0U));
			hashCombine(std::hash<u32>()(static_cast<u32>(_key.srcColor)));
			hashCombine(std::hash<u32>()(static_cast<u32>(_key.dstColor)));
			hashCombine(std::hash<u32>()(static_cast<u32>(_key.srcAlpha)));
			hashCombine(std::hash<u32>()(static_cast<u32>(_key.dstAlpha)));
			hashCombine(std::hash<u32>()(_key.depthTestEnabled ? 1U : 0U));
			hashCombine(std::hash<u32>()(_key.depthWriteEnabled ? 1U : 0U));
			hashCombine(std::hash<u32>()(static_cast<u32>(_key.depthCompare)));
			hashCombine(std::hash<u32>()(static_cast<u32>(_key.cullMode)));
			hashCombine(std::hash<u32>()(_key.textured ? 1U : 0U));
			return hashValue;
		}
	};

	static VkPrimitiveTopology _toVkTopology(PrimitiveType _primitive)
	{
		switch (_primitive) {
		case PrimitiveType::Triangles:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		case PrimitiveType::TriangleStrip:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
		case PrimitiveType::TriangleFan:
			return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
		case PrimitiveType::Lines:
			return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
		}
		return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	}

	static VkBlendFactor _toVkBlendFactor(BlendFactor _factor)
	{
		switch (_factor) {
		case BlendFactor::Zero:
			return VK_BLEND_FACTOR_ZERO;
		case BlendFactor::One:
			return VK_BLEND_FACTOR_ONE;
		case BlendFactor::SrcAlpha:
			return VK_BLEND_FACTOR_SRC_ALPHA;
		case BlendFactor::OneMinusSrcAlpha:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		case BlendFactor::DstAlpha:
			return VK_BLEND_FACTOR_DST_ALPHA;
		case BlendFactor::ConstantAlpha:
			return VK_BLEND_FACTOR_CONSTANT_ALPHA;
		case BlendFactor::OneMinusConstantAlpha:
			return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
		case BlendFactor::Src1Color:
			return VK_BLEND_FACTOR_SRC1_COLOR;
		case BlendFactor::OneMinusSrc1Color:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
		case BlendFactor::Src1Alpha:
			return VK_BLEND_FACTOR_SRC1_ALPHA;
		case BlendFactor::OneMinusSrc1Alpha:
			return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
		}
		return VK_BLEND_FACTOR_ONE;
	}

	static VkCompareOp _toVkCompareOp(CompareMode _mode)
	{
		switch (_mode) {
		case CompareMode::kLess:
			return VK_COMPARE_OP_LESS;
		case CompareMode::kAlways:
			return VK_COMPARE_OP_ALWAYS;
		case CompareMode::kLessOrEqual:
		default:
			return VK_COMPARE_OP_LESS_OR_EQUAL;
		}
	}

	static VkCullModeFlags _toVkCullMode(CullMode _mode)
	{
		switch (_mode) {
		case CullMode::kFront:
			return VK_CULL_MODE_FRONT_BIT;
		case CullMode::kBack:
			return VK_CULL_MODE_BACK_BIT;
		case CullMode::kFrontAndBack:
			return VK_CULL_MODE_FRONT_AND_BACK;
		case CullMode::kNone:
		default:
			return VK_CULL_MODE_NONE;
		}
	}

	PipelineKey _buildKey(const DrawPacket & _packet) const
	{
		PipelineKey key{};
		static const bool forceBlendOff = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_FORCE_BLEND_OFF", false);
		static const bool forceAlphaBlend = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_FORCE_ALPHA_BLEND", false);
		static const bool forceDepthTest = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_FORCE_DEPTH_TEST", false);
		static const bool forceDepthWrite = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_FORCE_DEPTH_WRITE", false);
		static const bool disableFillDepthOnlyBlendFix = vulkan::env::flagEnabled("REALITYVK_VK_DISABLE_FILL_DEPTH_ONLY_BLEND_FIX", false);
		static const bool debugFillDepthOnlyBlendFix = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_FILL_DEPTH_ONLY_BLEND_FIX", false);
		static u32 debugFillDepthOnlyBlendFixCount = 0U;
		key.primitive = _packet.primitive;
		if (forceAlphaBlend) {
			key.blendEnabled = true;
			key.srcColor = BlendFactor::SrcAlpha;
			key.dstColor = BlendFactor::OneMinusSrcAlpha;
			key.srcAlpha = BlendFactor::One;
			key.dstAlpha = BlendFactor::Zero;
		} else {
			key.blendEnabled = forceBlendOff ? false : _packet.state.blend.enabled;
			key.srcColor = forceBlendOff ? BlendFactor::One : _packet.state.blend.srcColor;
			key.dstColor = forceBlendOff ? BlendFactor::Zero : _packet.state.blend.dstColor;
				key.srcAlpha = forceBlendOff ? BlendFactor::One : _packet.state.blend.srcAlpha;
				key.dstAlpha = forceBlendOff ? BlendFactor::Zero : _packet.state.blend.dstAlpha;
			}
		key.depthTestEnabled = forceDepthTest ? true : _packet.state.depth.testEnabled;
		if (forceDepthWrite) {
			key.depthTestEnabled = true;
			key.depthWriteEnabled = true;
		} else {
			key.depthWriteEnabled = key.depthTestEnabled && _packet.state.depth.writeEnabled;
		}
		key.depthCompare = _packet.state.depth.compare;
		key.cullMode = _packet.state.cullMode;
		key.textured = _packet.textureSlotMask != 0U;

		// Depth-clear style fill-cycle rects frequently carry alpha=0 and should preserve
		// existing color while still updating depth-related state.
		if (!disableFillDepthOnlyBlendFix
			&& !forceAlphaBlend
			&& !forceBlendOff
			&& _packet.transformMode == VertexTransformMode::Rect
			&& !_packet.debugTexrect
			&& _packet.debugCombinerCycleType == 3U /* G_CYC_FILL */
			&& _packet.textureSlotMask == 0U
			&& !_packet.vertices.empty()) {
			f32 maxAlpha = _packet.vertices[0].a;
			for (const DrawVertex & vertex : _packet.vertices)
				maxAlpha = std::max(maxAlpha, vertex.a);
			if (maxAlpha <= 0.001f) {
				key.blendEnabled = true;
				key.srcColor = BlendFactor::Zero;
				key.dstColor = BlendFactor::One;
				key.srcAlpha = BlendFactor::Zero;
				key.dstAlpha = BlendFactor::One;
				if (debugFillDepthOnlyBlendFix && debugFillDepthOnlyBlendFixCount < 64U) {
					LOG(
						LOG_WARNING,
						"VK fill-depth blend fix: mux=0x%016llx cycle=%u alphaMax=%.3f depth=[test=%u write=%u]",
						static_cast<unsigned long long>(_packet.debugCombinerMux),
						_packet.debugCombinerCycleType,
						maxAlpha,
						_packet.state.depth.testEnabled ? 1U : 0U,
						_packet.state.depth.writeEnabled ? 1U : 0U);
					++debugFillDepthOnlyBlendFixCount;
				}
			}
		}

		return key;
	}

	VkPipeline _createPipeline(const PipelineKey & _key) const
	{
		const bool useTexturedShader = _key.textured && m_texturedVertexShader != VK_NULL_HANDLE && m_texturedFragmentShader != VK_NULL_HANDLE;
		const VkShaderModule vertexShader = useTexturedShader ? m_texturedVertexShader : m_colorVertexShader;
		const VkShaderModule fragmentShader = useTexturedShader ? m_texturedFragmentShader : m_colorFragmentShader;
		if (vertexShader == VK_NULL_HANDLE || fragmentShader == VK_NULL_HANDLE)
			return VK_NULL_HANDLE;

		const VkPipelineShaderStageCreateInfo shaderStages[2] = {
			{
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				nullptr,
				0,
				VK_SHADER_STAGE_VERTEX_BIT,
				vertexShader,
				"main",
				nullptr
			},
			{
				VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				nullptr,
				0,
				VK_SHADER_STAGE_FRAGMENT_BIT,
				fragmentShader,
				"main",
				nullptr
			}
		};

		const VkVertexInputBindingDescription bindingDescription = {
			0,
			static_cast<u32>(sizeof(DrawVertex)),
			VK_VERTEX_INPUT_RATE_VERTEX
		};
		std::array<VkVertexInputAttributeDescription, 4> attributeDescriptions{};
		attributeDescriptions[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
		attributeDescriptions[1] = {
			1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, static_cast<u32>(offsetof(DrawVertex, r))
		};
		u32 attributeCount = 2;
		if (useTexturedShader) {
			attributeDescriptions[2] = {
				2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<u32>(offsetof(DrawVertex, s0))
			};
			attributeDescriptions[3] = {
				3, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<u32>(offsetof(DrawVertex, s1))
			};
			attributeCount = 4;
		}
		VkPipelineVertexInputStateCreateInfo vertexInputStateCreateInfo{};
		vertexInputStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputStateCreateInfo.vertexBindingDescriptionCount = 1;
		vertexInputStateCreateInfo.pVertexBindingDescriptions = &bindingDescription;
		vertexInputStateCreateInfo.vertexAttributeDescriptionCount = attributeCount;
		vertexInputStateCreateInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

		VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCreateInfo{};
		inputAssemblyStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssemblyStateCreateInfo.topology = _toVkTopology(_key.primitive);
		inputAssemblyStateCreateInfo.primitiveRestartEnable = VK_FALSE;

		VkPipelineViewportStateCreateInfo viewportStateCreateInfo{};
		viewportStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportStateCreateInfo.viewportCount = 1;
		viewportStateCreateInfo.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo rasterizationStateCreateInfo{};
		rasterizationStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizationStateCreateInfo.depthClampEnable = VK_FALSE;
		rasterizationStateCreateInfo.rasterizerDiscardEnable = VK_FALSE;
		rasterizationStateCreateInfo.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizationStateCreateInfo.cullMode = _toVkCullMode(_key.cullMode);
		// RealityVK packet coordinates follow GL-style winding expectations.
		// With Vulkan's viewport space convention in this backend, matching GL
		// cull behavior requires clockwise front faces.
		rasterizationStateCreateInfo.frontFace = VK_FRONT_FACE_CLOCKWISE;
		rasterizationStateCreateInfo.depthBiasEnable = VK_TRUE;
		rasterizationStateCreateInfo.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisampleStateCreateInfo{};
		multisampleStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampleStateCreateInfo.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		multisampleStateCreateInfo.sampleShadingEnable = VK_FALSE;

		VkPipelineDepthStencilStateCreateInfo depthStencilStateCreateInfo{};
		depthStencilStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencilStateCreateInfo.depthTestEnable = _key.depthTestEnabled ? VK_TRUE : VK_FALSE;
		depthStencilStateCreateInfo.depthWriteEnable = (_key.depthTestEnabled && _key.depthWriteEnabled) ? VK_TRUE : VK_FALSE;
		depthStencilStateCreateInfo.depthCompareOp = _toVkCompareOp(_key.depthCompare);
		depthStencilStateCreateInfo.depthBoundsTestEnable = VK_FALSE;
		depthStencilStateCreateInfo.stencilTestEnable = VK_FALSE;

		VkPipelineColorBlendAttachmentState colorBlendAttachmentState{};
		colorBlendAttachmentState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT
			| VK_COLOR_COMPONENT_G_BIT
			| VK_COLOR_COMPONENT_B_BIT
			| VK_COLOR_COMPONENT_A_BIT;
		colorBlendAttachmentState.blendEnable = _key.blendEnabled ? VK_TRUE : VK_FALSE;
		colorBlendAttachmentState.srcColorBlendFactor = _toVkBlendFactor(_key.srcColor);
		colorBlendAttachmentState.dstColorBlendFactor = _toVkBlendFactor(_key.dstColor);
		colorBlendAttachmentState.colorBlendOp = VK_BLEND_OP_ADD;
		colorBlendAttachmentState.srcAlphaBlendFactor = _toVkBlendFactor(_key.srcAlpha);
		colorBlendAttachmentState.dstAlphaBlendFactor = _toVkBlendFactor(_key.dstAlpha);
		colorBlendAttachmentState.alphaBlendOp = VK_BLEND_OP_ADD;

		VkPipelineColorBlendStateCreateInfo colorBlendStateCreateInfo{};
		colorBlendStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlendStateCreateInfo.logicOpEnable = VK_FALSE;
		colorBlendStateCreateInfo.attachmentCount = 1;
		colorBlendStateCreateInfo.pAttachments = &colorBlendAttachmentState;

		const VkDynamicState dynamicStates[] = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_SCISSOR,
			VK_DYNAMIC_STATE_LINE_WIDTH,
			VK_DYNAMIC_STATE_BLEND_CONSTANTS,
			VK_DYNAMIC_STATE_DEPTH_BIAS
		};
		VkPipelineDynamicStateCreateInfo dynamicStateCreateInfo{};
		dynamicStateCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicStateCreateInfo.dynamicStateCount = 5;
		dynamicStateCreateInfo.pDynamicStates = dynamicStates;

		VkGraphicsPipelineCreateInfo pipelineCreateInfo{};
		pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineCreateInfo.stageCount = 2;
		pipelineCreateInfo.pStages = shaderStages;
		pipelineCreateInfo.pVertexInputState = &vertexInputStateCreateInfo;
		pipelineCreateInfo.pInputAssemblyState = &inputAssemblyStateCreateInfo;
		pipelineCreateInfo.pViewportState = &viewportStateCreateInfo;
		pipelineCreateInfo.pRasterizationState = &rasterizationStateCreateInfo;
		pipelineCreateInfo.pMultisampleState = &multisampleStateCreateInfo;
		pipelineCreateInfo.pDepthStencilState = &depthStencilStateCreateInfo;
		pipelineCreateInfo.pColorBlendState = &colorBlendStateCreateInfo;
		pipelineCreateInfo.pDynamicState = &dynamicStateCreateInfo;
		pipelineCreateInfo.layout = m_layout;
		pipelineCreateInfo.renderPass = m_renderPass;
		pipelineCreateInfo.subpass = 0;

		VkPipeline pipeline = VK_NULL_HANDLE;
		if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, nullptr, &pipeline) != VK_SUCCESS)
			return VK_NULL_HANDLE;
		return pipeline;
	}

	VkDevice m_device = VK_NULL_HANDLE;
	VkRenderPass m_renderPass = VK_NULL_HANDLE;
	VkPipelineLayout m_layout = VK_NULL_HANDLE;
	VkShaderModule m_colorVertexShader = VK_NULL_HANDLE;
	VkShaderModule m_colorFragmentShader = VK_NULL_HANDLE;
	VkShaderModule m_texturedVertexShader = VK_NULL_HANDLE;
	VkShaderModule m_texturedFragmentShader = VK_NULL_HANDLE;
	std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> m_pipelines;
};
#else
class PipelineCache
{
};
#endif

} // namespace vulkan
