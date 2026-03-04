#pragma once

#include <array>
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <Types.h>
#include "vulkan_DescriptorBinder.h"
#include "vulkan_DrawRecorder.h"
#include "vulkan_DrawShaderConfig.h"
#include "vulkan_Env.h"
#include "vulkan_PipelineCache.h"
#include <Log.h>

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
class DrawCommandEncoder
{
public:
	struct EncodeInfo {
		VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
		VkBuffer vertexBuffer = VK_NULL_HANDLE;
		VkDeviceSize vertexBufferOffset = 0;
		const std::vector<DrawPacket> * packets = nullptr;
		PipelineCache * pipelineCache = nullptr;
		DescriptorBinder * descriptorBinder = nullptr;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkExtent2D renderExtent = {};
		bool wideLinesEnabled = false;
		f32 maxLineWidth = 1.0f;
	};

	static void encode(const EncodeInfo & _info)
	{
		if (_info.commandBuffer == VK_NULL_HANDLE
			|| _info.vertexBuffer == VK_NULL_HANDLE
			|| _info.packets == nullptr
			|| _info.pipelineCache == nullptr
			|| _info.packets->empty()) {
			return;
		}

		vkCmdBindVertexBuffers(_info.commandBuffer, 0, 1, &_info.vertexBuffer, &_info.vertexBufferOffset);
		VkPipeline boundPipeline = VK_NULL_HANDLE;
		bool hasAppliedState = false;
		f32 appliedLineWidth = 1.0f;
		std::array<f32, 4> appliedBlendColor = { -1.0f, -1.0f, -1.0f, -1.0f };
		bool appliedDepthBiasEnabled = false;
		f32 appliedDepthBiasSlope = 0.0f;
		f32 appliedDepthBiasConstant = 0.0f;
		u32 firstVertex = 0;
		u32 skippedPackets = 0;
		u32 drawnPackets = 0;
		u32 zeroScissorPackets = 0;
		u32 texturedPackets = 0;
			static const bool debugEncoder = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_ENCODER", false);
			static const bool debugBlendState = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_BLEND_STATE", false);
			static const u32 debugBlendStateLimit = []() -> u32 {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_BLEND_STATE_LIMIT");
				if (env == nullptr || env[0] == '\0')
					return 96U;
				return static_cast<u32>(std::strtoul(env, nullptr, 10));
			}();
			static const bool debugStrictBlendState = vulkan::env::flagEnabled("REALITYVK_VK_DEBUG_STRICT_BLEND_STATE", false);
		static const u32 debugStrictBlendStateLimit = []() -> u32 {
			const char * env = std::getenv("REALITYVK_VK_DEBUG_STRICT_BLEND_STATE_LIMIT");
			if (env == nullptr || env[0] == '\0')
				return 96U;
			return static_cast<u32>(std::strtoul(env, nullptr, 10));
		}();

		for (const DrawPacket & packet : *_info.packets) {
			const u32 vertexCount = static_cast<u32>(packet.vertices.size());
			if (vertexCount == 0)
				continue;
			if (packet.textureSlotMask != 0U)
				++texturedPackets;
			if (packet.state.raster.scissorEnabled
				&& (packet.state.raster.scissorWidth <= 0 || packet.state.raster.scissorHeight <= 0)) {
				++zeroScissorPackets;
			}

			VkPipeline pipeline = _info.pipelineCache->getOrCreate(packet);
			if (pipeline == VK_NULL_HANDLE) {
				++skippedPackets;
				firstVertex += vertexCount;
				continue;
			}
			if (pipeline != boundPipeline) {
				vkCmdBindPipeline(_info.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				boundPipeline = pipeline;
				hasAppliedState = false;
			}
				if (_info.pipelineLayout != VK_NULL_HANDLE) {
					DrawPushConstants pushConstants{};
				pushConstants.flags = packet.shaderFlags;
				pushConstants.texrectAlphaTest = packet.texrectAlphaTest;
				pushConstants.texrectFilterMode = packet.texrectFilterMode;
				pushConstants.reserved0 = 0U;
				pushConstants.texrectTextureWidth = packet.texrectTextureWidth;
				pushConstants.texrectTextureHeight = packet.texrectTextureHeight;
				pushConstants.gammaLevel = packet.gammaLevel;
				pushConstants.textColorR = packet.textColorR;
				pushConstants.textColorG = packet.textColorG;
				pushConstants.textColorB = packet.textColorB;
				pushConstants.textColorA = packet.textColorA;
				pushConstants.fogColorR = packet.fogColorR;
				pushConstants.fogColorG = packet.fogColorG;
				pushConstants.fogColorB = packet.fogColorB;
				pushConstants.fogColorA = packet.fogColorA;
				if ((packet.shaderFlags & draw_shader_flags::kSpecialStrictBlendMux) != 0U) {
					pushConstants.texrectAlphaTest = packet.blendMux1Packed;
					pushConstants.texrectFilterMode = packet.blendMux2Packed;
					pushConstants.reserved0 = packet.blendParamsPacked;
					pushConstants.reserved1 = 0.0f;
					if ((packet.shaderFlags & draw_shader_flags::kSpecialTextDraw) == 0U) {
						pushConstants.textColorR = packet.state.blend.blendColor[0];
						pushConstants.textColorG = packet.state.blend.blendColor[1];
						pushConstants.textColorB = packet.state.blend.blendColor[2];
						pushConstants.textColorA = packet.state.blend.blendColor[3];
					}
					if (debugStrictBlendState) {
						struct StrictBlendStateCounter {
							u32 mux1 = 0U;
							u32 mux2 = 0U;
							u32 params = 0U;
							BlendFactor srcColor = BlendFactor::One;
							BlendFactor dstColor = BlendFactor::Zero;
							BlendFactor srcAlpha = BlendFactor::One;
							BlendFactor dstAlpha = BlendFactor::Zero;
							u32 count = 0U;
						};
						static std::vector<StrictBlendStateCounter> counters;
						static u32 logCount = 0U;

						bool found = false;
						for (StrictBlendStateCounter & entry : counters) {
							if (entry.mux1 == packet.blendMux1Packed
								&& entry.mux2 == packet.blendMux2Packed
								&& entry.params == packet.blendParamsPacked
								&& entry.srcColor == packet.state.blend.srcColor
								&& entry.dstColor == packet.state.blend.dstColor
								&& entry.srcAlpha == packet.state.blend.srcAlpha
								&& entry.dstAlpha == packet.state.blend.dstAlpha) {
								entry.count += 1U;
								if (logCount < debugStrictBlendStateLimit
									&& (entry.count == 1U || (entry.count & (entry.count - 1U)) == 0U)) {
									LOG(
										LOG_WARNING,
										"VK strict blend state: mux1=0x%02x mux2=0x%02x params=0x%02x blendEnabled=%u srcC=%u dstC=%u srcA=%u dstA=%u count=%u",
										entry.mux1,
										entry.mux2,
										entry.params,
										packet.state.blend.enabled ? 1U : 0U,
										static_cast<u32>(entry.srcColor),
										static_cast<u32>(entry.dstColor),
										static_cast<u32>(entry.srcAlpha),
										static_cast<u32>(entry.dstAlpha),
										entry.count);
									++logCount;
								}
								found = true;
								break;
							}
						}
						if (!found) {
							StrictBlendStateCounter entry{};
							entry.mux1 = packet.blendMux1Packed;
							entry.mux2 = packet.blendMux2Packed;
							entry.params = packet.blendParamsPacked;
							entry.srcColor = packet.state.blend.srcColor;
							entry.dstColor = packet.state.blend.dstColor;
							entry.srcAlpha = packet.state.blend.srcAlpha;
							entry.dstAlpha = packet.state.blend.dstAlpha;
							entry.count = 1U;
							counters.push_back(entry);
							if (logCount < debugStrictBlendStateLimit) {
								LOG(
									LOG_WARNING,
									"VK strict blend state: new mux1=0x%02x mux2=0x%02x params=0x%02x blendEnabled=%u srcC=%u dstC=%u srcA=%u dstA=%u tracked=%u",
									entry.mux1,
									entry.mux2,
									entry.params,
									packet.state.blend.enabled ? 1U : 0U,
									static_cast<u32>(entry.srcColor),
									static_cast<u32>(entry.dstColor),
									static_cast<u32>(entry.srcAlpha),
									static_cast<u32>(entry.dstAlpha),
									static_cast<u32>(counters.size()));
								++logCount;
							}
						}
					}
				}
					vkCmdPushConstants(
						_info.commandBuffer,
						_info.pipelineLayout,
						VK_SHADER_STAGE_FRAGMENT_BIT,
						0,
						static_cast<u32>(sizeof(DrawPushConstants)),
						&pushConstants);
				}
				if (debugBlendState) {
					struct BlendStateCounter {
						u32 shaderFlags = 0U;
						bool enabled = false;
						BlendFactor srcColor = BlendFactor::One;
						BlendFactor dstColor = BlendFactor::Zero;
						BlendFactor srcAlpha = BlendFactor::One;
						BlendFactor dstAlpha = BlendFactor::Zero;
						u32 count = 0U;
					};
					static std::vector<BlendStateCounter> counters;
					static u32 logCount = 0U;
					bool found = false;
					for (BlendStateCounter & entry : counters) {
						if (entry.shaderFlags == packet.shaderFlags
							&& entry.enabled == packet.state.blend.enabled
							&& entry.srcColor == packet.state.blend.srcColor
							&& entry.dstColor == packet.state.blend.dstColor
							&& entry.srcAlpha == packet.state.blend.srcAlpha
							&& entry.dstAlpha == packet.state.blend.dstAlpha) {
							entry.count += 1U;
							if (logCount < debugBlendStateLimit
								&& (entry.count == 1U || (entry.count & (entry.count - 1U)) == 0U)) {
								LOG(
									LOG_WARNING,
									"VK blend state: shaderFlags=0x%08x blendEnabled=%u srcC=%u dstC=%u srcA=%u dstA=%u count=%u",
									entry.shaderFlags,
									entry.enabled ? 1U : 0U,
									static_cast<u32>(entry.srcColor),
									static_cast<u32>(entry.dstColor),
									static_cast<u32>(entry.srcAlpha),
									static_cast<u32>(entry.dstAlpha),
									entry.count);
								++logCount;
							}
							found = true;
							break;
						}
					}
					if (!found) {
						BlendStateCounter entry{};
						entry.shaderFlags = packet.shaderFlags;
						entry.enabled = packet.state.blend.enabled;
						entry.srcColor = packet.state.blend.srcColor;
						entry.dstColor = packet.state.blend.dstColor;
						entry.srcAlpha = packet.state.blend.srcAlpha;
						entry.dstAlpha = packet.state.blend.dstAlpha;
						entry.count = 1U;
						counters.push_back(entry);
						if (logCount < debugBlendStateLimit) {
							LOG(
								LOG_WARNING,
								"VK blend state: new shaderFlags=0x%08x blendEnabled=%u srcC=%u dstC=%u srcA=%u dstA=%u tracked=%u",
								entry.shaderFlags,
								entry.enabled ? 1U : 0U,
								static_cast<u32>(entry.srcColor),
								static_cast<u32>(entry.dstColor),
								static_cast<u32>(entry.srcAlpha),
								static_cast<u32>(entry.dstAlpha),
								static_cast<u32>(counters.size()));
							++logCount;
						}
					}
				}
				if (_info.descriptorBinder != nullptr && _info.pipelineLayout != VK_NULL_HANDLE)
					_info.descriptorBinder->bindPacket(_info.commandBuffer, _info.pipelineLayout, packet);

			u32 dirtyMask = packet.dirtyMask;
			if (!hasAppliedState)
				dirtyMask |= draw_dirty::kAll;

			if ((dirtyMask & draw_dirty::kViewport) != 0U)
				_setViewport(_info.commandBuffer, packet.state.raster, _info.renderExtent);
			if ((dirtyMask & draw_dirty::kScissor) != 0U)
				_setScissor(_info.commandBuffer, packet.state.raster, _info.renderExtent);
			if ((dirtyMask & draw_dirty::kBlend) != 0U || !hasAppliedState) {
				const std::array<f32, 4> blendColor = packet.state.blend.blendColor;
				if (!hasAppliedState
					|| blendColor[0] != appliedBlendColor[0]
					|| blendColor[1] != appliedBlendColor[1]
					|| blendColor[2] != appliedBlendColor[2]
					|| blendColor[3] != appliedBlendColor[3]) {
					vkCmdSetBlendConstants(_info.commandBuffer, blendColor.data());
					appliedBlendColor = blendColor;
				}
			}
			if ((dirtyMask & draw_dirty::kDepth) != 0U || !hasAppliedState) {
				const bool depthBiasEnabled = packet.state.depth.polygonOffsetEnabled;
				const f32 depthBiasSlope = depthBiasEnabled ? packet.state.depth.polygonOffsetFactor : 0.0f;
				const f32 depthBiasConstant = depthBiasEnabled ? packet.state.depth.polygonOffsetUnits : 0.0f;
				if (!hasAppliedState
					|| depthBiasEnabled != appliedDepthBiasEnabled
					|| depthBiasSlope != appliedDepthBiasSlope
					|| depthBiasConstant != appliedDepthBiasConstant) {
					vkCmdSetDepthBias(_info.commandBuffer, depthBiasConstant, 0.0f, depthBiasSlope);
					appliedDepthBiasEnabled = depthBiasEnabled;
					appliedDepthBiasSlope = depthBiasSlope;
					appliedDepthBiasConstant = depthBiasConstant;
				}
			}

			if (packet.primitive == PrimitiveType::Lines) {
				const f32 lineWidth = _info.wideLinesEnabled
					? std::min(std::max(1.0f, packet.lineWidth), _info.maxLineWidth)
					: 1.0f;
				if ((dirtyMask & draw_dirty::kLineWidth) != 0U || !hasAppliedState || lineWidth != appliedLineWidth) {
					vkCmdSetLineWidth(_info.commandBuffer, lineWidth);
					appliedLineWidth = lineWidth;
				}
			}

			vkCmdDraw(_info.commandBuffer, vertexCount, 1, firstVertex, 0);
			++drawnPackets;
			firstVertex += vertexCount;
			hasAppliedState = true;
		}
		if (debugEncoder) {
			const DrawPacket * firstPacket = _info.packets->empty() ? nullptr : &(*_info.packets)[0];
			LOG(
				LOG_WARNING,
				"VK encoder debug: packets=%u drawn=%u skipped=%u textured=%u zeroScissor=%u firstVP=%d,%d,%d,%d firstScissor=%d,%d,%d,%d enabled=%u",
				static_cast<unsigned>(_info.packets->size()),
				drawnPackets,
				skippedPackets,
				texturedPackets,
				zeroScissorPackets,
				firstPacket != nullptr ? firstPacket->state.raster.viewportX : 0,
				firstPacket != nullptr ? firstPacket->state.raster.viewportY : 0,
				firstPacket != nullptr ? firstPacket->state.raster.viewportWidth : 0,
				firstPacket != nullptr ? firstPacket->state.raster.viewportHeight : 0,
				firstPacket != nullptr ? firstPacket->state.raster.scissorX : 0,
				firstPacket != nullptr ? firstPacket->state.raster.scissorY : 0,
				firstPacket != nullptr ? firstPacket->state.raster.scissorWidth : 0,
				firstPacket != nullptr ? firstPacket->state.raster.scissorHeight : 0,
				firstPacket != nullptr && firstPacket->state.raster.scissorEnabled ? 1U : 0U);
		}
	}

private:
	static void _setViewport(VkCommandBuffer _commandBuffer, const RasterState & _raster, VkExtent2D _renderExtent)
	{
		VkViewport viewport{};
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;
		if (_raster.viewportValid && _raster.viewportWidth > 0 && _raster.viewportHeight > 0) {
			viewport.x = static_cast<f32>(std::max<s32>(0, _raster.viewportX));
			viewport.y = static_cast<f32>(std::max<s32>(0, _raster.viewportY));
			viewport.width = static_cast<f32>(std::max<s32>(1, _raster.viewportWidth));
			viewport.height = static_cast<f32>(std::max<s32>(1, _raster.viewportHeight));
		} else {
			viewport.x = 0.0f;
			viewport.y = 0.0f;
			viewport.width = static_cast<f32>(_renderExtent.width);
			viewport.height = static_cast<f32>(_renderExtent.height);
		}
		vkCmdSetViewport(_commandBuffer, 0, 1, &viewport);
	}

	static void _setScissor(VkCommandBuffer _commandBuffer, const RasterState & _raster, VkExtent2D _renderExtent)
	{
		VkRect2D scissor{};
		if (!_raster.scissorEnabled) {
			scissor.offset = { 0, 0 };
			scissor.extent = _renderExtent;
			vkCmdSetScissor(_commandBuffer, 0, 1, &scissor);
			return;
		}

		const s32 sx = _raster.scissorX;
		const s32 sy = _raster.scissorY;
		const s32 sw = _raster.scissorWidth;
		const s32 sh = _raster.scissorHeight;
		if (sw <= 0 || sh <= 0) {
			scissor.offset = { 0, 0 };
			scissor.extent = { 0, 0 };
			vkCmdSetScissor(_commandBuffer, 0, 1, &scissor);
			return;
		}

		const s32 maxX = static_cast<s32>(_renderExtent.width);
		const s32 maxY = static_cast<s32>(_renderExtent.height);
		const s32 clippedX = std::max<s32>(0, std::min<s32>(sx, maxX));
		const s32 clippedY = std::max<s32>(0, std::min<s32>(sy, maxY));

		if (clippedX >= maxX || clippedY >= maxY) {
			scissor.offset = { 0, 0 };
			scissor.extent = { 0, 0 };
			vkCmdSetScissor(_commandBuffer, 0, 1, &scissor);
			return;
		}

		scissor.offset.x = clippedX;
		scissor.offset.y = clippedY;
		const u32 availableWidth = _renderExtent.width - static_cast<u32>(clippedX);
		const u32 availableHeight = _renderExtent.height - static_cast<u32>(clippedY);
		scissor.extent.width = std::min<u32>(static_cast<u32>(sw), availableWidth);
		scissor.extent.height = std::min<u32>(static_cast<u32>(sh), availableHeight);
		vkCmdSetScissor(_commandBuffer, 0, 1, &scissor);
	}
};
#else
class DrawCommandEncoder
{
};
#endif

} // namespace vulkan
