#include "vulkan_ContextImpl_Internal.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <cstdio>
#include <cinttypes>
#include <vector>
#include <Log.h>
#include <Graphics/ColorBufferReader.h>
#include <Graphics/CombinerProgram.h>
#include <Graphics/Parameters.h>
#include <Graphics/ShaderProgram.h>
#include <gDP.h>
#include <gSP.h>
#include "vulkan_DrawCommandEncoder.h"
#include "vulkan_ShaderKeyStorage.h"
#include "vulkan_CombinerHeuristics.h"
#include "vulkan_CombinerApply.h"
#include "vulkan_BlendMux.h"
#include "vulkan_ReadbackAdapters.h"
#include "vulkan_PacketBuilder.h"
#include "vulkan_PacketNormalize.h"
#include "vulkan_SpecialPrograms.h"

namespace {

#if REALITYVK_VULKAN_HEADERS_AVAILABLE
const char * packetSourceName(u32 _source);

void attachPacketBindings(const vulkan::BindingState & _bindingState, vulkan::DrawPacket & _packet)
{
	static const bool debugBindings = std::getenv("REALITYVK_VK_DEBUG_BINDINGS") != nullptr;
	static const bool forceUntextured = std::getenv("REALITYVK_VK_DEBUG_FORCE_UNTEXTURED") != nullptr;
	static const bool forceTexture0Only = std::getenv("REALITYVK_VK_DEBUG_FORCE_TEXTURE0_ONLY") != nullptr;
	static const bool forceTexturedNoSample = std::getenv("REALITYVK_VK_DEBUG_FORCE_TEXTURED_NO_SAMPLE") != nullptr;
	static const char * forcedTextureHandleEnv = std::getenv("REALITYVK_VK_DEBUG_FORCE_TEXTURE_HANDLE");
	static const u32 forcedTextureHandle = forcedTextureHandleEnv != nullptr && forcedTextureHandleEnv[0] != '\0'
		? static_cast<u32>(std::strtoul(forcedTextureHandleEnv, nullptr, 10))
		: 0U;
	static bool loggedDebugConfig = false;
	if (!loggedDebugConfig
		&& (debugBindings || forceUntextured || forceTexture0Only || forceTexturedNoSample || forcedTextureHandle != 0U)) {
		LOG(
			LOG_WARNING,
			"VK bindings debug config: enabled=%u forceUntextured=%u forceTexture0Only=%u forceTexturedNoSample=%u forcedTextureHandle=%u",
			debugBindings ? 1U : 0U,
			forceUntextured ? 1U : 0U,
			forceTexture0Only ? 1U : 0U,
			forceTexturedNoSample ? 1U : 0U,
			forcedTextureHandle);
		loggedDebugConfig = true;
	}
	_packet.textureSlotMask = 0U;
	std::array<vulkan::BindingState::TextureBinding, vulkan::binding_limits::kTextureUnits> textureBindings{};
	u32 availableTextureUnitMask = 0U;
	_bindingState.forEachTextureBinding([&textureBindings, &availableTextureUnitMask](u32 _unit, const vulkan::BindingState::TextureBinding & _binding) {
		if (_unit >= vulkan::binding_limits::kTextureUnits)
			return;
		textureBindings[_unit] = _binding;
		availableTextureUnitMask |= (1U << _unit);
	});
	auto assignTextureInput = [&](u32 _descriptorIndex, u32 _unit) {
		if (_descriptorIndex >= vulkan::binding_limits::kTextureUnits || _unit >= vulkan::binding_limits::kTextureUnits)
			return;
		if ((availableTextureUnitMask & (1U << _unit)) == 0U)
			return;
		const vulkan::BindingState::TextureBinding & binding = textureBindings[_unit];
		if (!binding.texture.isNotNull())
			return;
		vulkan::TextureSlotReference & textureRef = _packet.textureSlots[_descriptorIndex];
		textureRef.unit = _descriptorIndex;
		textureRef.texture = binding.texture;
		textureRef.target = binding.target;
		_packet.textureSlotMask |= (1U << _descriptorIndex);
	};
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kTexture0) != 0U)
		assignTextureInput(0, _packet.textureUnit0);
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kTexture1) != 0U)
		assignTextureInput(1, _packet.textureUnit1);
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kSpecialDepthFog) != 0U) {
		assignTextureInput(3U, static_cast<u32>(graphics::textureIndices::ZLUTTex));
		assignTextureInput(4U, static_cast<u32>(graphics::textureIndices::PaletteTex));
	}
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kTexture0) != 0U && (_packet.textureSlotMask & (1U << 0)) == 0U)
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kTexture0;
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kTexture1) != 0U && (_packet.textureSlotMask & (1U << 1)) == 0U)
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kTexture1;
	if ((_packet.shaderFlags & vulkan::draw_shader_flags::kSpecialDepthFog) != 0U
		&& ((_packet.textureSlotMask & (1U << 3U)) == 0U || (_packet.textureSlotMask & (1U << 4U)) == 0U)) {
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kSpecialDepthFog;
	}
	if (forceUntextured) {
		_packet.textureSlotMask = 0U;
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kTexture1);
	}
	if (forceTexture0Only) {
		_packet.textureSlotMask &= ~(1U << 1);
		_packet.shaderFlags &= ~vulkan::draw_shader_flags::kTexture1;
	}
	if (forceTexturedNoSample && _packet.textureSlotMask != 0U) {
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture0 | vulkan::draw_shader_flags::kTexture1);
		_packet.shaderFlags |= vulkan::draw_shader_flags::kShade;
	}
	if (forcedTextureHandle != 0U) {
		_packet.textureSlots[0].unit = 0U;
		_packet.textureSlots[0].texture = graphics::ObjectHandle(forcedTextureHandle);
		_packet.textureSlotMask |= (1U << 0);
		_packet.textureSlotMask &= ~(1U << 1);
		_packet.shaderFlags &= ~(vulkan::draw_shader_flags::kTexture1);
		_packet.shaderFlags |= vulkan::draw_shader_flags::kTexture0;
		for (vulkan::DrawVertex & vertex : _packet.vertices) {
			vertex.s0 = 0.5f;
			vertex.t0 = 0.5f;
		}
	}
		if (debugBindings) {
			static const u32 packetBindingLogLimit = []() -> u32 {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_BINDINGS_PACKET_LIMIT");
				if (env == nullptr || env[0] == '\0')
					return 96U;
				return static_cast<u32>(std::strtoul(env, nullptr, 10));
			}();
			static u32 packetBindingLogCount = 0U;
			if (packetBindingLogCount < packetBindingLogLimit && _packet.textureSlotMask != 0U) {
			const u32 handle0 = (_packet.textureSlotMask & (1U << 0)) != 0U
				? static_cast<u32>(_packet.textureSlots[0].texture)
				: 0U;
			const u32 handle1 = (_packet.textureSlotMask & (1U << 1)) != 0U
				? static_cast<u32>(_packet.textureSlots[1].texture)
				: 0U;
			const u32 bound0 = textureBindings[0].texture.isNotNull() ? static_cast<u32>(textureBindings[0].texture) : 0U;
			const u32 bound1 = textureBindings[1].texture.isNotNull() ? static_cast<u32>(textureBindings[1].texture) : 0U;
			const u32 bound2 = textureBindings[2].texture.isNotNull() ? static_cast<u32>(textureBindings[2].texture) : 0U;
			const u32 bound3 = textureBindings[3].texture.isNotNull() ? static_cast<u32>(textureBindings[3].texture) : 0U;
			f32 minS0 = 0.0f;
			f32 maxS0 = 0.0f;
			f32 minT0 = 0.0f;
			f32 maxT0 = 0.0f;
			f32 minS1 = 0.0f;
			f32 maxS1 = 0.0f;
			f32 minT1 = 0.0f;
			f32 maxT1 = 0.0f;
			if (!_packet.vertices.empty()) {
				minS0 = maxS0 = _packet.vertices[0].s0;
				minT0 = maxT0 = _packet.vertices[0].t0;
				minS1 = maxS1 = _packet.vertices[0].s1;
				minT1 = maxT1 = _packet.vertices[0].t1;
				for (const vulkan::DrawVertex & vertex : _packet.vertices) {
					minS0 = std::min(minS0, vertex.s0);
					maxS0 = std::max(maxS0, vertex.s0);
					minT0 = std::min(minT0, vertex.t0);
					maxT0 = std::max(maxT0, vertex.t0);
					minS1 = std::min(minS1, vertex.s1);
					maxS1 = std::max(maxS1, vertex.s1);
					minT1 = std::min(minT1, vertex.t1);
					maxT1 = std::max(maxT1, vertex.t1);
				}
			}
				LOG(
					LOG_WARNING,
					"VK bindings debug: packet id=%llu src=%s texrect=%u mux=0x%016llx cycle=%u shaderFlags=0x%08x textureMask=0x%08x unit0=%u handle0=%u unit1=%u handle1=%u bound=[u0:%u u1:%u u2:%u u3:%u] tex0[s=%0.3f..%0.3f t=%0.3f..%0.3f] tex1[s=%0.3f..%0.3f t=%0.3f..%0.3f]",
					static_cast<unsigned long long>(_packet.debugPacketId),
					packetSourceName(_packet.debugSource),
					_packet.debugTexrect ? 1U : 0U,
					static_cast<unsigned long long>(_packet.debugCombinerMux),
					_packet.debugCombinerCycleType,
					_packet.shaderFlags,
					_packet.textureSlotMask,
					_packet.textureSlots[0].unit,
				handle0,
				_packet.textureSlots[1].unit,
				handle1,
				bound0,
				bound1,
				bound2,
				bound3,
				minS0,
				maxS0,
				minT0,
				maxT0,
				minS1,
				maxS1,
				minT1,
				maxT1);
			++packetBindingLogCount;
		}
		const bool requestedTexture0 = (_packet.shaderFlags & vulkan::draw_shader_flags::kTexture0) != 0U;
		const bool requestedTexture1 = (_packet.shaderFlags & vulkan::draw_shader_flags::kTexture1) != 0U;
		if ((requestedTexture0 || requestedTexture1) && _packet.textureSlotMask == 0U) {
			LOG(
				LOG_WARNING,
				"VK bindings debug: requested textures (t0=%u,t1=%u) but no bound texture slots available (unitMask=0x%08x).",
				requestedTexture0 ? 1U : 0U,
				requestedTexture1 ? 1U : 0U,
				availableTextureUnitMask);
		}
	}

	_packet.imageSlotMask = 0U;
	_bindingState.forEachImageBinding([&_packet](u32 _unit, const vulkan::BindingState::ImageBinding & _binding) {
		if (_unit >= vulkan::binding_limits::kImageUnits)
			return;
		vulkan::ImageSlotReference & imageRef = _packet.imageSlots[_unit];
		imageRef.unit = _unit;
		imageRef.texture = _binding.texture;
		imageRef.accessMode = _binding.accessMode;
		imageRef.textureFormat = _binding.textureFormat;
		_packet.imageSlotMask |= (1U << _unit);
	});
}

void normalizePacketTextureCoordinates(const vulkan::TextureStore & _textureStore, vulkan::DrawPacket & _packet)
{
	static const bool debugBindings = std::getenv("REALITYVK_VK_DEBUG_BINDINGS") != nullptr;
	static const bool disableNormalize = std::getenv("REALITYVK_VK_DISABLE_TEXCOORD_NORMALIZE") != nullptr;
	static const bool enableFixedPointTexcoordScale = std::getenv("REALITYVK_VK_ENABLE_TEXCOORD_FIXEDPOINT_SCALE") != nullptr;
	if (disableNormalize)
		return;

	auto normalizeSlot = [&](u32 _slot, bool _useFirstTexCoordSet) {
		if ((_packet.textureSlotMask & (1U << _slot)) == 0U)
			return;
		const vulkan::TextureSlotReference & slotRef = _packet.textureSlots[_slot];
		const vulkan::TextureStore::TextureResource * texture = _textureStore.getTexture(slotRef.texture);
		if (texture == nullptr || texture->width == 0U || texture->height == 0U)
			return;
		// Convert texel-space coordinates against the backing image dimensions.
		// Using tracked content bounds here can over-normalize and effectively zoom samples.
		const u32 texWidth = texture->width;
		const u32 texHeight = texture->height;
		if (texWidth == 0U || texHeight == 0U)
			return;

		f32 maxAbsS = 0.0f;
		f32 maxAbsT = 0.0f;
		for (const vulkan::DrawVertex & vertex : _packet.vertices) {
			const f32 s = _useFirstTexCoordSet ? vertex.s0 : vertex.s1;
			const f32 t = _useFirstTexCoordSet ? vertex.t0 : vertex.t1;
			maxAbsS = std::max(maxAbsS, std::abs(s));
			maxAbsT = std::max(maxAbsT, std::abs(t));
		}

		// N64/RealityVK feeds many texture coordinates in texel space.
		// Convert likely texel-space values to normalized UVs for sampler2D shaders.
		if (maxAbsS <= 2.0f && maxAbsT <= 2.0f)
			return;
			f32 texcoordScale = 1.0f;
			// Some N64 texrect paths still deliver s10.5 fixed-point texture coordinates.
			// Apply an extra 1/32 scale when the coordinates are implausibly large for the
			// currently bound texture dimensions.
			if (enableFixedPointTexcoordScale
				&& (maxAbsS > static_cast<f32>(texWidth * 2U)
					|| maxAbsT > static_cast<f32>(texHeight * 2U))) {
				texcoordScale = 1.0f / 32.0f;
			}
			const f32 invW = texcoordScale / static_cast<f32>(texWidth);
			const f32 invH = texcoordScale / static_cast<f32>(texHeight);
		for (vulkan::DrawVertex & vertex : _packet.vertices) {
			if (_useFirstTexCoordSet) {
				vertex.s0 *= invW;
				vertex.t0 *= invH;
			} else {
				vertex.s1 *= invW;
				vertex.t1 *= invH;
			}
		}
			if (debugBindings) {
				static const u32 normalizeLogLimit = []() -> u32 {
					const char * env = std::getenv("REALITYVK_VK_DEBUG_BINDINGS_NORMALIZE_LIMIT");
					if (env == nullptr || env[0] == '\0')
						return 96U;
					return static_cast<u32>(std::strtoul(env, nullptr, 10));
				}();
				static u32 normalizeLogCount = 0U;
				if (normalizeLogCount < normalizeLogLimit) {
				f32 minS = _useFirstTexCoordSet ? _packet.vertices[0].s0 : _packet.vertices[0].s1;
				f32 maxS = minS;
				f32 minT = _useFirstTexCoordSet ? _packet.vertices[0].t0 : _packet.vertices[0].t1;
				f32 maxT = minT;
				for (const vulkan::DrawVertex & vertex : _packet.vertices) {
					const f32 s = _useFirstTexCoordSet ? vertex.s0 : vertex.s1;
					const f32 t = _useFirstTexCoordSet ? vertex.t0 : vertex.t1;
					minS = std::min(minS, s);
					maxS = std::max(maxS, s);
					minT = std::min(minT, t);
					maxT = std::max(maxT, t);
				}
					LOG(
						LOG_WARNING,
						"VK bindings debug: normalized texcoords slot=%u handle=%u preMaxAbs=[%0.3f,%0.3f] post=[s=%0.3f..%0.3f t=%0.3f..%0.3f] texSize=%ux%u contentSize=%ux%u",
						_slot,
						static_cast<u32>(slotRef.texture),
						maxAbsS,
						maxAbsT,
						minS,
						maxS,
						minT,
						maxT,
						texture->width,
						texture->height,
						texture->contentWidth,
						texture->contentHeight);
					++normalizeLogCount;
				}
			}
		};

	normalizeSlot(0U, true);
	normalizeSlot(1U, false);
}

bool resolvePrimitiveType(graphics::DrawModeParam _mode, vulkan::PrimitiveType & _outPrimitive)
{
	if (_mode == graphics::drawmode::TRIANGLES) {
		_outPrimitive = vulkan::PrimitiveType::Triangles;
		return true;
	}
	if (_mode == graphics::drawmode::TRIANGLE_STRIP) {
		_outPrimitive = vulkan::PrimitiveType::TriangleStrip;
		return true;
	}
	if (_mode == graphics::drawmode::TRIANGLE_FAN) {
		_outPrimitive = vulkan::PrimitiveType::TriangleFan;
		return true;
	}
	return false;
}

const char * bufferTargetName(graphics::BufferTargetParam _target)
{
	if (_target == graphics::bufferTarget::FRAMEBUFFER)
		return "FRAMEBUFFER";
	if (_target == graphics::bufferTarget::DRAW_FRAMEBUFFER)
		return "DRAW_FRAMEBUFFER";
	if (_target == graphics::bufferTarget::READ_FRAMEBUFFER)
		return "READ_FRAMEBUFFER";
	return "UNKNOWN";
}

const char * bufferAttachmentName(graphics::BufferAttachmentParam _attachment)
{
	if (_attachment == graphics::bufferAttachment::COLOR_ATTACHMENT0)
		return "COLOR_ATTACHMENT0";
	if (_attachment == graphics::bufferAttachment::COLOR_ATTACHMENT1)
		return "COLOR_ATTACHMENT1";
	if (_attachment == graphics::bufferAttachment::COLOR_ATTACHMENT2)
		return "COLOR_ATTACHMENT2";
	if (_attachment == graphics::bufferAttachment::COLOR_ATTACHMENT3)
		return "COLOR_ATTACHMENT3";
	if (_attachment == graphics::bufferAttachment::COLOR_ATTACHMENT4)
		return "COLOR_ATTACHMENT4";
	if (_attachment == graphics::bufferAttachment::DEPTH_ATTACHMENT)
		return "DEPTH_ATTACHMENT";
	return "UNKNOWN";
}

bool isVkFboTraceEnabled()
{
	static const bool traceEnabled = std::getenv("REALITYVK_VK_TRACE_FBO") != nullptr;
	return traceEnabled;
}

bool isVkFboTraceVerboseEnabled()
{
	static const bool traceVerboseEnabled = std::getenv("REALITYVK_VK_TRACE_FBO_VERBOSE") != nullptr;
	return traceVerboseEnabled;
}

u32 vkFboTraceLimit()
{
	static const u32 traceLimit = []() -> u32 {
		const char * envLimit = std::getenv("REALITYVK_VK_TRACE_FBO_LIMIT");
		if (envLimit == nullptr || envLimit[0] == '\0')
			return 2000U;
		const u32 parsed = static_cast<u32>(std::strtoul(envLimit, nullptr, 10));
		return parsed == 0U ? 2000U : parsed;
	}();
	return traceLimit;
}

void vkFboTrace(const char * _fmt, ...)
{
	if (!isVkFboTraceEnabled())
		return;
	static u32 emittedCount = 0U;
	const u32 limit = vkFboTraceLimit();
	if (emittedCount >= limit)
		return;
	++emittedCount;
	char message[768];
	va_list args;
	va_start(args, _fmt);
	std::vsnprintf(message, sizeof(message), _fmt, args);
	va_end(args);
	LOG(LOG_WARNING, "VK FBO TRACE: %s", message);
	if (emittedCount == limit) {
		LOG(LOG_WARNING, "VK FBO TRACE: limit reached (%u); suppressing further FBO trace logs.", limit);
	}
}

u64 & offscreenSkippedDrawCalls()
{
	static u64 value = 0U;
	return value;
}

u64 & offscreenSkippedVertices()
{
	static u64 value = 0U;
	return value;
}

enum class DepthBlitFailureReason : u8 {
	kNone = 0,
	UnsupportedDefaultTarget,
	MissingAttachment,
	MissingTextureHandle,
	InvalidTextureResource,
	FormatMismatch,
	EmptyRegion,
	BackendFailure
};

struct DepthBlitStats {
	u64 attempts = 0U;
	u64 successes = 0U;
	u64 failures = 0U;
	u64 unsupportedDefaultTarget = 0U;
	u64 missingAttachment = 0U;
	u64 missingTextureHandle = 0U;
	u64 invalidTextureResource = 0U;
	u64 formatMismatch = 0U;
	u64 emptyRegion = 0U;
	u64 backendFailure = 0U;
};

DepthBlitStats & depthBlitStats()
{
	static DepthBlitStats stats{};
	return stats;
}

void recordDepthBlitFailure(DepthBlitFailureReason _reason)
{
	DepthBlitStats & stats = depthBlitStats();
	++stats.failures;
	switch (_reason) {
	case DepthBlitFailureReason::UnsupportedDefaultTarget:
		++stats.unsupportedDefaultTarget;
		break;
	case DepthBlitFailureReason::MissingAttachment:
		++stats.missingAttachment;
		break;
	case DepthBlitFailureReason::MissingTextureHandle:
		++stats.missingTextureHandle;
		break;
	case DepthBlitFailureReason::InvalidTextureResource:
		++stats.invalidTextureResource;
		break;
	case DepthBlitFailureReason::FormatMismatch:
		++stats.formatMismatch;
		break;
	case DepthBlitFailureReason::EmptyRegion:
		++stats.emptyRegion;
		break;
	case DepthBlitFailureReason::BackendFailure:
		++stats.backendFailure;
		break;
	case DepthBlitFailureReason::kNone:
	default:
		break;
	}
}

const char * depthBlitFailureReasonName(DepthBlitFailureReason _reason)
{
	switch (_reason) {
	case DepthBlitFailureReason::kNone:
		return "none";
	case DepthBlitFailureReason::UnsupportedDefaultTarget:
		return "unsupported_default_target";
	case DepthBlitFailureReason::MissingAttachment:
		return "missing_attachment";
	case DepthBlitFailureReason::MissingTextureHandle:
		return "missing_texture_handle";
	case DepthBlitFailureReason::InvalidTextureResource:
		return "invalid_texture_resource";
	case DepthBlitFailureReason::FormatMismatch:
		return "format_mismatch";
	case DepthBlitFailureReason::EmptyRegion:
		return "empty_region";
	case DepthBlitFailureReason::BackendFailure:
		return "backend_failure";
	default:
		return "unknown";
	}
}

const std::array<graphics::BufferAttachmentParam, 5> & colorAttachmentCandidates()
{
	static const std::array<graphics::BufferAttachmentParam, 5> candidates = {
		graphics::bufferAttachment::COLOR_ATTACHMENT0,
		graphics::bufferAttachment::COLOR_ATTACHMENT1,
		graphics::bufferAttachment::COLOR_ATTACHMENT2,
		graphics::bufferAttachment::COLOR_ATTACHMENT3,
		graphics::bufferAttachment::COLOR_ATTACHMENT4
	};
	return candidates;
}

bool isColorAttachment(graphics::BufferAttachmentParam _attachment)
{
	for (graphics::BufferAttachmentParam candidate : colorAttachmentCandidates()) {
		if (candidate == _attachment)
			return true;
	}
	return false;
}

const vulkan::FramebufferStore::FramebufferAttachment * resolveColorAttachment(
	const vulkan::FramebufferStore & _store,
	graphics::ObjectHandle _framebuffer,
	graphics::BufferAttachmentParam _preferredAttachment,
	graphics::BufferAttachmentParam * _resolvedAttachment,
	u32 _maxColorAttachments = 5U)
{
	if (_resolvedAttachment != nullptr)
		*_resolvedAttachment = graphics::bufferAttachment::COLOR_ATTACHMENT0;
	if (!_framebuffer.isNotNull())
		return nullptr;
	const auto & candidates = colorAttachmentCandidates();
	const u32 availableCandidates = static_cast<u32>(candidates.size());
	const u32 primaryCandidateCount = std::max<u32>(
		1U,
		std::min<u32>(_maxColorAttachments, availableCandidates));

	auto isPrimaryCandidate = [&](graphics::BufferAttachmentParam _attachment) -> bool {
		for (u32 i = 0U; i < primaryCandidateCount; ++i) {
			if (candidates[i] == _attachment)
				return true;
		}
		return false;
	};

	auto tryAttachment = [&](graphics::BufferAttachmentParam _attachment) -> const vulkan::FramebufferStore::FramebufferAttachment * {
		const vulkan::FramebufferStore::FramebufferAttachment * attachment = _store.getAttachment(_framebuffer, _attachment);
		if (attachment == nullptr || !attachment->textureHandle.isNotNull())
			return nullptr;
		if (_resolvedAttachment != nullptr)
			*_resolvedAttachment = _attachment;
		return attachment;
	};

	if (isColorAttachment(_preferredAttachment) && isPrimaryCandidate(_preferredAttachment)) {
		if (const vulkan::FramebufferStore::FramebufferAttachment * preferred = tryAttachment(_preferredAttachment))
			return preferred;
	}
	for (u32 i = 0U; i < primaryCandidateCount; ++i) {
		const graphics::BufferAttachmentParam candidate = candidates[i];
		if (candidate == _preferredAttachment)
			continue;
		if (const vulkan::FramebufferStore::FramebufferAttachment * attachment = tryAttachment(candidate))
			return attachment;
	}
	for (u32 i = primaryCandidateCount; i < availableCandidates; ++i) {
		const graphics::BufferAttachmentParam candidate = candidates[i];
		if (candidate == _preferredAttachment)
			continue;
		if (const vulkan::FramebufferStore::FramebufferAttachment * attachment = tryAttachment(candidate))
			return attachment;
	}
	return nullptr;
}

bool maskIncludes(graphics::Parameter _mask, graphics::BlitMaskParam _flag)
{
	return (static_cast<u32>(_mask) & static_cast<u32>(_flag)) != 0U;
}

const char * transformModeName(vulkan::VertexTransformMode _mode)
{
	if (_mode == vulkan::VertexTransformMode::Rect)
		return "rect";
	return "triangle";
}

enum : u32 {
	kPacketSourceUnknown = 0U,
	kPacketSourceTriangles = 1U,
	kPacketSourceRects = 2U,
	kPacketSourceLines = 3U,
	kPacketSourceBlitDefault = 4U
};

u64 nextVkDrawPacketId()
{
	static u64 value = 1U;
	return value++;
}

const char * packetSourceName(u32 _source)
{
	switch (_source) {
	case kPacketSourceTriangles:
		return "triangles";
	case kPacketSourceRects:
		return "rects";
	case kPacketSourceLines:
		return "lines";
	case kPacketSourceBlitDefault:
		return "blit_default";
	default:
		return "unknown";
	}
}

void assignPacketDebugMetadata(
	vulkan::DrawPacket & _packet,
	u32 _source,
	const graphics::CombinerProgram * _combiner,
	bool _texrect)
{
	_packet.debugPacketId = nextVkDrawPacketId();
	_packet.debugSource = _source;
	_packet.debugTexrect = _texrect;
	if (_combiner != nullptr) {
		const CombinerKey & key = _combiner->getKey();
		_packet.debugCombinerMux = static_cast<u64>(key.getMux());
		_packet.debugCombinerCycleType = key.getCycleType();
	} else {
		_packet.debugCombinerMux = 0U;
		_packet.debugCombinerCycleType = 0U;
	}
}

#endif

} // namespace

namespace vulkan {

ContextImpl::ContextImpl()
	: m_clampMode(graphics::ClampMode::ClippingEnabled)
	, m_textureUnpackAlignment(1)
	, m_maxTextureSize(4096)
	, m_maxMsaaLevel(1)
	, m_maxLineWidth(1.0f)
	, m_maxAnisotropy(1.0f)
	, m_nextHandle(1U)
	, m_coreReady(false)
	, m_presentationWindowInfo()
{
}

ContextImpl::~ContextImpl()
{
	destroy();
}

bool ContextImpl::hasVulkanSupport()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	return true;
#else
	return false;
#endif
}

void ContextImpl::setPresentationWindowInfo(const graphics::Context::PresentationWindowInfo & _info)
{
	const bool changed = m_presentationWindowInfo.system != _info.system
		|| m_presentationWindowInfo.display != _info.display
		|| m_presentationWindowInfo.window != _info.window
		|| m_presentationWindowInfo.width != _info.width
		|| m_presentationWindowInfo.height != _info.height;
	m_presentationWindowInfo = _info;

#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!changed || !m_vk || m_vk->instance == VK_NULL_HANDLE)
		return;

	if (m_vk->device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_vk->device);

	destroySwapchain();
	destroySurface();
	if (createSurface())
		createSwapchain();
#endif
}

void ContextImpl::init()
{
	initFramebufferFormats();
	if (!initializeVulkanCore()) {
		LOG(LOG_WARNING, "Vulkan core init failed. Backend remains in bootstrap mode.");
	}
}

void ContextImpl::destroy()
{
	shutdownVulkanCore();
}

void ContextImpl::setClampMode(graphics::ClampMode _mode)
{
	m_clampMode = _mode;
}

graphics::ClampMode ContextImpl::getClampMode()
{
	return m_clampMode;
}

void ContextImpl::enable(graphics::EnableParam _parameter, bool _enable)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.enable(_parameter, _enable);
#else
	(void)_parameter;
	(void)_enable;
#endif
}

u32 ContextImpl::isEnabled(graphics::EnableParam _parameter)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return 0U;
	return m_vk->drawRecorder.isEnabled(_parameter);
#else
	(void)_parameter;
	return 0U;
#endif
}

void ContextImpl::cullFace(graphics::CullModeParam _mode)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.cullFace(_mode);
#else
	(void)_mode;
#endif
}

void ContextImpl::enableDepthWrite(bool _enable)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.enableDepthWrite(_enable);
#else
	(void)_enable;
#endif
}

void ContextImpl::setDepthCompare(graphics::CompareParam _mode)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setDepthCompare(_mode);
#else
	(void)_mode;
#endif
}

void ContextImpl::setViewport(s32 _x, s32 _y, s32 _width, s32 _height)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setViewport(_x, _y, _width, _height);
#else
	(void)_x;
	(void)_y;
	(void)_width;
	(void)_height;
#endif
}

void ContextImpl::setScissor(s32 _x, s32 _y, s32 _width, s32 _height)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setScissor(_x, _y, _width, _height);
#else
	(void)_x;
	(void)_y;
	(void)_width;
	(void)_height;
#endif
}

void ContextImpl::setBlending(graphics::BlendParam _sfactor, graphics::BlendParam _dfactor)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setBlending(_sfactor, _dfactor);
#else
	(void)_sfactor;
	(void)_dfactor;
#endif
}

void ContextImpl::setBlendingSeparate(graphics::BlendParam _sfactorcolor, graphics::BlendParam _dfactorcolor, graphics::BlendParam _sfactoralpha, graphics::BlendParam _dfactoralpha)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setBlendingSeparate(_sfactorcolor, _dfactorcolor, _sfactoralpha, _dfactoralpha);
#else
	(void)_sfactorcolor;
	(void)_dfactorcolor;
	(void)_sfactoralpha;
	(void)_dfactoralpha;
#endif
}

void ContextImpl::setBlendColor(f32 _red, f32 _green, f32 _blue, f32 _alpha)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setBlendColor(_red, _green, _blue, _alpha);
#else
	(void)_red;
	(void)_green;
	(void)_blue;
	(void)_alpha;
#endif
}

void ContextImpl::clearColorBuffer(f32 _red, f32 _green, f32 _blue, f32 _alpha)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	if (isDefaultDrawFramebufferBound()) {
		m_vk->drawRecorder.setClearColor(_red, _green, _blue, _alpha);
		vkFboTrace(
			"op=clear_color target=default color=[%.3f,%.3f,%.3f,%.3f]",
			_red,
			_green,
			_blue,
			_alpha);
		return;
	}
	const graphics::ObjectHandle drawFramebuffer = m_vk->drawRecorder.drawFramebufferBinding();
	const FramebufferStore::FramebufferResource * framebuffer = m_vk->framebufferStore.getFramebuffer(drawFramebuffer);
	if (framebuffer == nullptr) {
		vkFboTrace(
			"op=clear_color target=fbo drawFbo=%u status=missing_framebuffer",
			static_cast<u32>(drawFramebuffer));
		return;
	}
	u32 clearedAttachments = 0U;
	for (const auto & attachmentEntry : framebuffer->attachments) {
		const FramebufferStore::FramebufferAttachment & attachment = attachmentEntry.second;
		if (!isColorAttachment(attachment.attachment))
			continue;
		if (!attachment.textureHandle.isNotNull())
			continue;
		const bool clearOk = m_vk->textureStore.clearTextureColor(attachment.textureHandle, _red, _green, _blue, _alpha);
		++clearedAttachments;
		vkFboTrace(
			"op=clear_color_attachment drawFbo=%u attachment=%s texture=%u ok=%u color=[%.3f,%.3f,%.3f,%.3f]",
			static_cast<u32>(drawFramebuffer),
			bufferAttachmentName(attachment.attachment),
			static_cast<u32>(attachment.textureHandle),
			clearOk ? 1U : 0U,
			_red,
			_green,
			_blue,
			_alpha);
	}
	vkFboTrace(
		"op=clear_color target=fbo drawFbo=%u touched=%u",
		static_cast<u32>(drawFramebuffer),
		clearedAttachments);
#else
	(void)_red;
	(void)_green;
	(void)_blue;
	(void)_alpha;
#endif
}

void ContextImpl::clearDepthBuffer()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	if (isDefaultDrawFramebufferBound()) {
		m_vk->drawRecorder.setClearDepth(1.0f);
		vkFboTrace("op=clear_depth target=default depth=1.000");
		return;
	}
	const graphics::ObjectHandle drawFramebuffer = m_vk->drawRecorder.drawFramebufferBinding();
	const FramebufferStore::FramebufferAttachment * depthAttachment = m_vk->framebufferStore.getAttachment(
		drawFramebuffer,
		graphics::bufferAttachment::DEPTH_ATTACHMENT);
	if (depthAttachment == nullptr || !depthAttachment->textureHandle.isNotNull()) {
		vkFboTrace(
			"op=clear_depth target=fbo drawFbo=%u status=missing_depth_attachment",
			static_cast<u32>(drawFramebuffer));
		return;
	}
	const bool clearOk = m_vk->textureStore.clearTextureDepth(depthAttachment->textureHandle, 1.0f);
	vkFboTrace(
		"op=clear_depth target=fbo drawFbo=%u texture=%u ok=%u depth=1.000",
		static_cast<u32>(drawFramebuffer),
		static_cast<u32>(depthAttachment->textureHandle),
		clearOk ? 1U : 0U);
#endif
}

void ContextImpl::setPolygonOffset(f32 _factor, f32 _units)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.setPolygonOffset(_factor, _units);
#else
	(void)_factor;
	(void)_units;
#endif
}

graphics::ObjectHandle ContextImpl::createTexture(graphics::Parameter _target)
{
	graphics::ObjectHandle handle = _allocateHandle();
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (m_vk)
		m_vk->textureStore.createTexture(handle, _target);
#else
	(void)_target;
#endif
	return handle;
}

void ContextImpl::deleteTexture(graphics::ObjectHandle _name)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->textureStore.deleteTexture(_name);
	m_vk->bindingState.clearTextureBindings(_name);
#else
	(void)_name;
#endif
}

void ContextImpl::init2DTexture(const graphics::Context::InitTextureParams & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->textureStore.initTexture(_params, m_textureUnpackAlignment);
	if (_params.handle.isNotNull()) {
		graphics::Context::BindTextureParameters bindParams{};
		bindParams.texture = _params.handle;
		bindParams.textureUnitIndex = _params.textureUnitIndex;
		bindParams.target = _params.target;
		m_vk->bindingState.bindTexture(bindParams);
	}
#else
	(void)_params;
#endif
}

void ContextImpl::update2DTexture(const graphics::Context::UpdateTextureDataParams & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->textureStore.updateTexture(_params, m_textureUnpackAlignment);
	if (_params.handle.isNotNull()) {
		graphics::Context::BindTextureParameters bindParams{};
		bindParams.texture = _params.handle;
		bindParams.textureUnitIndex = _params.textureUnitIndex;
		bindParams.target = graphics::textureTarget::TEXTURE_2D;
		m_vk->bindingState.bindTexture(bindParams);
	}
#else
	(void)_params;
#endif
}

void ContextImpl::setTextureParameters(const graphics::Context::TexParameters & _parameters)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->textureStore.setTextureParameters(_parameters);
	graphics::Context::BindTextureParameters bindParams{};
	bindParams.texture = _parameters.handle;
	bindParams.textureUnitIndex = _parameters.textureUnitIndex;
	bindParams.target = _parameters.target;
	m_vk->bindingState.bindTexture(bindParams);
#else
	(void)_parameters;
#endif
}

void ContextImpl::bindTexture(const graphics::Context::BindTextureParameters & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	static const bool debugBindings = std::getenv("REALITYVK_VK_DEBUG_BINDINGS") != nullptr;
	if (debugBindings) {
		static u32 debugBindTextureLogCount = 0U;
		if (debugBindTextureLogCount < 64U) {
			LOG(
				LOG_WARNING,
				"VK bindings debug: bindTexture unit=%u texture=%u target=%u",
				static_cast<u32>(_params.textureUnitIndex),
				static_cast<u32>(_params.texture),
				static_cast<u32>(_params.target));
			++debugBindTextureLogCount;
		}
	}
	m_vk->textureStore.ensureTexture(_params.texture, _params.target);
	m_vk->bindingState.bindTexture(_params);
#else
	(void)_params;
#endif
}

void ContextImpl::setTextureUnpackAlignment(s32 _param)
{
	m_textureUnpackAlignment = _param;
}

s32 ContextImpl::getTextureUnpackAlignment() const
{
	return m_textureUnpackAlignment;
}

s32 ContextImpl::getMaxTextureSize() const
{
	return m_maxTextureSize;
}

f32 ContextImpl::getMaxAnisotropy() const
{
	return m_maxAnisotropy;
}

void ContextImpl::bindImageTexture(const graphics::Context::BindImageTextureParameters & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->bindingState.bindImageTexture(_params);
#else
	(void)_params;
#endif
}

u32 ContextImpl::convertInternalTextureFormat(u32 _format) const
{
	return _format;
}

void ContextImpl::textureBarrier()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	// Vulkan image layout transitions and explicit offscreen submissions already
	// provide visibility; keep this as a conservative ordering point.
	if (m_vk->device != VK_NULL_HANDLE)
		(void)vkDeviceWaitIdle(m_vk->device);
	m_vk->drawRecorder.markAllStateDirty();
#endif
}

graphics::FramebufferTextureFormats * ContextImpl::getFramebufferTextureFormats()
{
	return new graphics::FramebufferTextureFormats(*m_fbTexFormats);
}

graphics::ObjectHandle ContextImpl::createFramebuffer()
{
	graphics::ObjectHandle handle = _allocateHandle();
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (m_vk)
		m_vk->framebufferStore.createFramebuffer(handle);
#endif
	return handle;
}

void ContextImpl::deleteFramebuffer(graphics::ObjectHandle _name)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->framebufferStore.deleteFramebuffer(_name);
#else
	(void)_name;
#endif
}

void ContextImpl::bindFramebuffer(graphics::BufferTargetParam _target, graphics::ObjectHandle _name)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.bindFramebuffer(_target, _name);
	if (_name.isNotNull()) {
		if (_target == graphics::bufferTarget::FRAMEBUFFER || _target == graphics::bufferTarget::DRAW_FRAMEBUFFER) {
			m_vk->lastNonDefaultDrawFramebuffer = _name;
			m_vk->lastNonDefaultDrawFramebufferSerial = ++m_vk->framebufferBindSerial;
		}
		if (_target == graphics::bufferTarget::FRAMEBUFFER || _target == graphics::bufferTarget::READ_FRAMEBUFFER) {
			m_vk->lastNonDefaultReadFramebuffer = _name;
			m_vk->lastNonDefaultReadFramebufferSerial = ++m_vk->framebufferBindSerial;
		}
	}
	vkFboTrace(
		"op=bind target=%s name=%u draw=%u read=%u",
		bufferTargetName(_target),
		static_cast<u32>(_name),
		static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
		static_cast<u32>(m_vk->drawRecorder.readFramebufferBinding()));
#else
	(void)_target;
	(void)_name;
#endif
}

void ContextImpl::addFrameBufferRenderTarget(const graphics::Context::FrameBufferRenderTarget & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->framebufferStore.addFramebufferRenderTarget(_params);
	vkFboTrace(
		"op=attach fbo=%u target=%s attachment=%s texture=%u textureTarget=%u",
		static_cast<u32>(_params.bufferHandle),
		bufferTargetName(_params.bufferTarget),
		bufferAttachmentName(_params.attachment),
		static_cast<u32>(_params.textureHandle),
		static_cast<u32>(_params.textureTarget));
#else
	(void)_params;
#endif
}

graphics::ObjectHandle ContextImpl::createRenderbuffer()
{
	graphics::ObjectHandle handle = _allocateHandle();
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (m_vk)
		m_vk->framebufferStore.createRenderbuffer(handle);
#endif
	return handle;
}

void ContextImpl::initRenderbuffer(const graphics::Context::InitRenderbufferParams & _params)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->framebufferStore.initRenderbuffer(_params);
#else
	(void)_params;
#endif
}

bool ContextImpl::blitFramebuffers(const graphics::Context::BlitFramebuffersParams & _params)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_params;
	return false;
#else
	if (!m_vk)
		return false;
	vkFboTrace(
		"op=blit_begin readFbo=%u drawFbo=%u src=[%d,%d,%d,%d] dst=[%d,%d,%d,%d] mask=0x%x filter=0x%x",
		static_cast<u32>(_params.readBuffer),
		static_cast<u32>(_params.drawBuffer),
		_params.srcX0,
		_params.srcY0,
		_params.srcX1,
		_params.srcY1,
		_params.dstX0,
		_params.dstY0,
		_params.dstX1,
		_params.dstY1,
		static_cast<u32>(_params.mask),
		static_cast<u32>(_params.filter));
	m_vk->drawRecorder.bindFramebuffer(graphics::bufferTarget::READ_FRAMEBUFFER, _params.readBuffer);
	m_vk->drawRecorder.bindFramebuffer(graphics::bufferTarget::DRAW_FRAMEBUFFER, _params.drawBuffer);
	if (!_params.readBuffer.isNotNull())
		return false;
	const bool drawToDefault = _params.drawBuffer == graphics::ObjectHandle::defaultFramebuffer;
	if (!drawToDefault && !_params.drawBuffer.isNotNull())
		return false;

	const bool wantsColor = maskIncludes(_params.mask, graphics::blitMask::COLOR_BUFFER);
	const bool wantsDepth = maskIncludes(_params.mask, graphics::blitMask::DEPTH_BUFFER);
	if (!wantsColor && !wantsDepth)
		return false;
	const u32 readColorAttachmentLimit = m_vk->framebufferStore.getDrawBufferCount(_params.readBuffer);

		if (drawToDefault) {
			// Depth blit to the default framebuffer is handled by higher-level
			// default-target copy path. Do not enqueue partial color work and then fail.
			if (wantsDepth) {
				vkFboTrace(
					"op=blit_end readFbo=%u drawFbo=%u srcColorAttachment=%s colorOp=0 colorOk=0 depthOp=1 depthOk=0 depthReason=%s result=0 path=default_target_depth_copy_unavailable",
					static_cast<u32>(_params.readBuffer),
					static_cast<u32>(_params.drawBuffer),
					bufferAttachmentName(graphics::bufferAttachment::COLOR_ATTACHMENT0),
					depthBlitFailureReasonName(DepthBlitFailureReason::UnsupportedDefaultTarget));
				return false;
			}

			bool ok = true;
			bool colorOp = false;
			bool colorOk = false;
			graphics::BufferAttachmentParam srcColorAttachment = graphics::bufferAttachment::COLOR_ATTACHMENT0;
			bool depthOp = false;
		bool depthOk = false;
		DepthBlitFailureReason depthReason = DepthBlitFailureReason::kNone;

		if (wantsColor) {
			colorOp = true;
			const FramebufferStore::FramebufferAttachment * srcAttachment = resolveColorAttachment(
				m_vk->framebufferStore,
				_params.readBuffer,
				graphics::bufferAttachment::COLOR_ATTACHMENT0,
				&srcColorAttachment,
				readColorAttachmentLimit);
			const TextureStore::TextureResource * srcTexture = srcAttachment != nullptr
				? m_vk->textureStore.getTexture(srcAttachment->textureHandle)
				: nullptr;
			if (srcAttachment == nullptr
				|| !srcAttachment->textureHandle.isNotNull()
				|| srcTexture == nullptr
				|| srcTexture->width == 0U
				|| srcTexture->height == 0U) {
				ok = false;
			} else {
				DrawPacket packet{};
				packet.primitive = PrimitiveType::TriangleStrip;
				packet.transformMode = VertexTransformMode::Rect;
				packet.textureUnit0 = 0U;
				packet.textureUnit1 = 1U;
				packet.shaderFlags = vulkan::draw_shader_flags::kTexture0;
				assignPacketDebugMetadata(packet, kPacketSourceBlitDefault, nullptr, true);
				packet.textureSlotMask = (1U << 0);
				packet.textureSlots[0].unit = 0U;
				packet.textureSlots[0].texture = srcAttachment->textureHandle;
				packet.textureSlots[0].target = graphics::TextureTargetParam(static_cast<u32>(srcAttachment->textureTarget));
				packet.vertices.reserve(4U);

				auto addVertex = [&packet](f32 _x, f32 _y, f32 _s, f32 _t) {
					DrawVertex v{};
					v.x = _x;
					v.y = _y;
					v.z = 0.0f;
					v.w = 1.0f;
					v.r = 1.0f;
					v.g = 1.0f;
					v.b = 1.0f;
					v.a = 1.0f;
					v.s0 = _s;
					v.t0 = _t;
					v.s1 = _s;
					v.t1 = _t;
					packet.vertices.push_back(v);
				};

				const f32 dstX0 = static_cast<f32>(_params.dstX0);
				const f32 dstY0 = static_cast<f32>(_params.dstY0);
				const f32 dstX1 = static_cast<f32>(_params.dstX1);
				const f32 dstY1 = static_cast<f32>(_params.dstY1);
				const f32 srcX0 = static_cast<f32>(_params.srcX0);
				const f32 srcY0 = static_cast<f32>(_params.srcY0);
				const f32 srcX1 = static_cast<f32>(_params.srcX1);
				const f32 srcY1 = static_cast<f32>(_params.srcY1);
				addVertex(dstX0, dstY0, srcX0, srcY0);
				addVertex(dstX1, dstY0, srcX1, srcY0);
				addVertex(dstX0, dstY1, srcX0, srcY1);
				addVertex(dstX1, dstY1, srcX1, srcY1);

				m_vk->drawRecorder.applyStateToPacket(packet);
				packet.state.depth.testEnabled = false;
				packet.state.depth.writeEnabled = false;
				packet.state.depth.compare = CompareMode::kAlways;
				packet.state.blend.enabled = false;
				packet.state.cullMode = CullMode::kNone;
				packet.state.raster.scissorEnabled = false;
				packet.dirtyMask |= draw_dirty::kDepth | draw_dirty::kBlend | draw_dirty::kCull | draw_dirty::kScissor;

				normalizePacketTextureCoordinates(m_vk->textureStore, packet);
				vulkan::packet_normalize::normalizePacketPositions(packet);
				m_vk->drawRecorder.pushPacket(std::move(packet));
				colorOk = true;
			}
		}

		if (wantsDepth) {
			DepthBlitStats & stats = depthBlitStats();
			++stats.attempts;
			depthOp = true;
			depthOk = false;
			depthReason = DepthBlitFailureReason::UnsupportedDefaultTarget;
			recordDepthBlitFailure(depthReason);
			ok = false;
		}

		const DepthBlitStats & stats = depthBlitStats();
		vkFboTrace(
			"op=blit_end readFbo=%u drawFbo=%u srcColorAttachment=%s colorOp=%u colorOk=%u depthOp=%u depthOk=%u depthReason=%s result=%u path=enqueue_default depthStats=[attempts=%llu success=%llu fail=%llu]",
			static_cast<u32>(_params.readBuffer),
			static_cast<u32>(_params.drawBuffer),
			bufferAttachmentName(srcColorAttachment),
			colorOp ? 1U : 0U,
			colorOk ? 1U : 0U,
			depthOp ? 1U : 0U,
			depthOk ? 1U : 0U,
			depthBlitFailureReasonName(depthReason),
			ok ? 1U : 0U,
			static_cast<unsigned long long>(stats.attempts),
			static_cast<unsigned long long>(stats.successes),
			static_cast<unsigned long long>(stats.failures));
		return ok;
	}

	bool ok = true;
	bool colorOp = false;
	bool colorOk = false;
	graphics::BufferAttachmentParam srcColorAttachment = graphics::bufferAttachment::COLOR_ATTACHMENT0;
	graphics::BufferAttachmentParam dstColorAttachment = graphics::bufferAttachment::COLOR_ATTACHMENT0;
	const u32 drawColorAttachmentLimit = m_vk->framebufferStore.getDrawBufferCount(_params.drawBuffer);
	if (wantsColor) {
		colorOp = true;
		const FramebufferStore::FramebufferAttachment * srcAttachment = resolveColorAttachment(
			m_vk->framebufferStore,
			_params.readBuffer,
			graphics::bufferAttachment::COLOR_ATTACHMENT0,
			&srcColorAttachment,
			readColorAttachmentLimit);
		const FramebufferStore::FramebufferAttachment * dstAttachment = resolveColorAttachment(
			m_vk->framebufferStore,
			_params.drawBuffer,
			srcColorAttachment,
			&dstColorAttachment,
			drawColorAttachmentLimit);
		if (srcAttachment == nullptr
			|| dstAttachment == nullptr
			|| !srcAttachment->textureHandle.isNotNull()
			|| !dstAttachment->textureHandle.isNotNull()) {
			ok = false;
		} else {
			colorOk = m_vk->textureStore.blitTexture(
				srcAttachment->textureHandle,
				dstAttachment->textureHandle,
				_params.srcX0,
				_params.srcY0,
				_params.srcX1,
				_params.srcY1,
				_params.dstX0,
				_params.dstY0,
				_params.dstX1,
				_params.dstY1,
				graphics::TextureParam(static_cast<u32>(_params.filter)),
				false);
			ok = ok && colorOk;
			if (colorOk && _params.drawBuffer.isNotNull()) {
				m_vk->lastColorBlitDrawFramebuffer = _params.drawBuffer;
				m_vk->lastColorBlitDrawFramebufferSerial = ++m_vk->framebufferBindSerial;
			}
		}
	}

	bool depthOp = false;
	bool depthOk = false;
	DepthBlitFailureReason depthReason = DepthBlitFailureReason::kNone;
	if (wantsDepth) {
		depthOp = true;
		DepthBlitStats & stats = depthBlitStats();
		++stats.attempts;
		const FramebufferStore::FramebufferAttachment * srcAttachment = m_vk->framebufferStore.getAttachment(
			_params.readBuffer,
			graphics::bufferAttachment::DEPTH_ATTACHMENT);
		const FramebufferStore::FramebufferAttachment * dstAttachment = m_vk->framebufferStore.getAttachment(
			_params.drawBuffer,
			graphics::bufferAttachment::DEPTH_ATTACHMENT);
		graphics::ObjectHandle srcDepthTextureHandle = graphics::ObjectHandle::null;
		graphics::ObjectHandle dstDepthTextureHandle = graphics::ObjectHandle::null;
		s32 depthSrcX0 = _params.srcX0;
		s32 depthSrcY0 = _params.srcY0;
		s32 depthSrcX1 = _params.srcX1;
		s32 depthSrcY1 = _params.srcY1;
		s32 depthDstX0 = _params.dstX0;
		s32 depthDstY0 = _params.dstY0;
		s32 depthDstX1 = _params.dstX1;
		s32 depthDstY1 = _params.dstY1;
		if (srcAttachment == nullptr || dstAttachment == nullptr) {
			depthReason = DepthBlitFailureReason::MissingAttachment;
		} else if (!srcAttachment->textureHandle.isNotNull() || !dstAttachment->textureHandle.isNotNull()) {
			depthReason = DepthBlitFailureReason::MissingTextureHandle;
			srcDepthTextureHandle = srcAttachment->textureHandle;
			dstDepthTextureHandle = dstAttachment->textureHandle;
		} else {
			srcDepthTextureHandle = srcAttachment->textureHandle;
			dstDepthTextureHandle = dstAttachment->textureHandle;
			const TextureStore::TextureResource * srcTexture = m_vk->textureStore.getTexture(srcDepthTextureHandle);
			const TextureStore::TextureResource * dstTexture = m_vk->textureStore.getTexture(dstDepthTextureHandle);
			if (srcTexture == nullptr
				|| dstTexture == nullptr
				|| srcTexture->image == VK_NULL_HANDLE
				|| dstTexture->image == VK_NULL_HANDLE
				|| srcTexture->width == 0U
				|| srcTexture->height == 0U
				|| dstTexture->width == 0U
				|| dstTexture->height == 0U
				|| srcTexture->vkFormat == VK_FORMAT_UNDEFINED
				|| dstTexture->vkFormat == VK_FORMAT_UNDEFINED) {
				depthReason = DepthBlitFailureReason::InvalidTextureResource;
			} else if (!hasDepthComponent(srcTexture->vkFormat) || !hasDepthComponent(dstTexture->vkFormat)) {
				depthReason = DepthBlitFailureReason::FormatMismatch;
			} else {
				auto clampToDimension = [](s32 _coord, u32 _dimension) -> s32 {
					const s32 maxCoord = static_cast<s32>(_dimension);
					if (_coord < 0)
						return 0;
					if (_coord > maxCoord)
						return maxCoord;
					return _coord;
				};
				const s32 srcX0 = clampToDimension(_params.srcX0, srcTexture->width);
				const s32 srcY0 = clampToDimension(_params.srcY0, srcTexture->height);
				const s32 srcX1 = clampToDimension(_params.srcX1, srcTexture->width);
				const s32 srcY1 = clampToDimension(_params.srcY1, srcTexture->height);
				const s32 dstX0 = clampToDimension(_params.dstX0, dstTexture->width);
				const s32 dstY0 = clampToDimension(_params.dstY0, dstTexture->height);
				const s32 dstX1 = clampToDimension(_params.dstX1, dstTexture->width);
				const s32 dstY1 = clampToDimension(_params.dstY1, dstTexture->height);
				const s32 srcMinX = std::min(srcX0, srcX1);
				const s32 srcMinY = std::min(srcY0, srcY1);
				const s32 srcWidth = std::max(srcX0, srcX1) - srcMinX;
				const s32 srcHeight = std::max(srcY0, srcY1) - srcMinY;
				const s32 dstMinX = std::min(dstX0, dstX1);
				const s32 dstMinY = std::min(dstY0, dstY1);
				const s32 dstWidth = std::max(dstX0, dstX1) - dstMinX;
				const s32 dstHeight = std::max(dstY0, dstY1) - dstMinY;
				const s32 copyWidth = std::min(srcWidth, dstWidth);
				const s32 copyHeight = std::min(srcHeight, dstHeight);
				if (copyWidth <= 0 || copyHeight <= 0) {
					depthReason = DepthBlitFailureReason::EmptyRegion;
				} else {
					depthSrcX0 = srcMinX;
					depthSrcY0 = srcMinY;
					depthSrcX1 = srcMinX + copyWidth;
					depthSrcY1 = srcMinY + copyHeight;
					depthDstX0 = dstMinX;
					depthDstY0 = dstMinY;
					depthDstX1 = dstMinX + copyWidth;
					depthDstY1 = dstMinY + copyHeight;
					depthOk = m_vk->textureStore.blitTexture(
						srcDepthTextureHandle,
						dstDepthTextureHandle,
						depthSrcX0,
						depthSrcY0,
						depthSrcX1,
						depthSrcY1,
						depthDstX0,
						depthDstY0,
						depthDstX1,
						depthDstY1,
						graphics::TextureParam(static_cast<u32>(graphics::textureParameters::FILTER_NEAREST)),
						true);
					if (!depthOk)
						depthReason = DepthBlitFailureReason::BackendFailure;
				}
			}
		}
		if (depthOk) {
			++stats.successes;
		} else {
			if (depthReason == DepthBlitFailureReason::kNone)
				depthReason = DepthBlitFailureReason::BackendFailure;
			recordDepthBlitFailure(depthReason);
			vkFboTrace(
				"op=blit_depth_fail readFbo=%u drawFbo=%u srcDepthTexture=%u dstDepthTexture=%u reason=%s src=[%d,%d,%d,%d] dst=[%d,%d,%d,%d]",
				static_cast<u32>(_params.readBuffer),
				static_cast<u32>(_params.drawBuffer),
				static_cast<u32>(srcDepthTextureHandle),
				static_cast<u32>(dstDepthTextureHandle),
				depthBlitFailureReasonName(depthReason),
				depthSrcX0,
				depthSrcY0,
				depthSrcX1,
				depthSrcY1,
				depthDstX0,
				depthDstY0,
				depthDstX1,
				depthDstY1);
			ok = false;
		}
		ok = ok && depthOk;
	}

	const DepthBlitStats & depthStats = depthBlitStats();
	vkFboTrace(
		"op=blit_end readFbo=%u drawFbo=%u srcColorAttachment=%s dstColorAttachment=%s colorOp=%u colorOk=%u depthOp=%u depthOk=%u depthReason=%s result=%u depthStats=[attempts=%llu success=%llu fail=%llu]",
		static_cast<u32>(_params.readBuffer),
		static_cast<u32>(_params.drawBuffer),
		bufferAttachmentName(srcColorAttachment),
		bufferAttachmentName(dstColorAttachment),
		colorOp ? 1U : 0U,
		colorOk ? 1U : 0U,
		depthOp ? 1U : 0U,
		depthOk ? 1U : 0U,
		depthBlitFailureReasonName(depthReason),
		ok ? 1U : 0U,
		static_cast<unsigned long long>(depthStats.attempts),
		static_cast<unsigned long long>(depthStats.successes),
		static_cast<unsigned long long>(depthStats.failures));
	return ok;
#endif
}

void ContextImpl::setDrawBuffers(u32 _num)
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	const graphics::ObjectHandle drawFramebuffer = m_vk->drawRecorder.drawFramebufferBinding();
	m_vk->framebufferStore.setDrawBuffers(drawFramebuffer, _num);
	vkFboTrace(
		"op=set_draw_buffers drawFbo=%u count=%u",
		static_cast<u32>(drawFramebuffer),
		std::max<u32>(1U, _num));
#else
	(void)_num;
#endif
}

bool ContextImpl::readScreen2(void * _dest, int _width, int _height, int _front)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_dest;
	(void)_width;
	(void)_height;
	(void)_front;
	return false;
#else
	(void)_front;
	static const bool debugReadScreen = std::getenv("REALITYVK_VK_DEBUG_READSCREEN") != nullptr;
	if (_dest == nullptr || _width <= 0 || _height <= 0)
		return false;
	if (!m_coreReady || !m_vk)
		return false;
	if (m_vk->device == VK_NULL_HANDLE
		|| m_vk->commandPool == VK_NULL_HANDLE
		|| m_vk->graphicsQueue == VK_NULL_HANDLE
		|| m_vk->swapchain == VK_NULL_HANDLE
		|| m_vk->swapchainImages.empty()) {
		return false;
	}
	if (m_vk->swapchainExtent.width == 0 || m_vk->swapchainExtent.height == 0)
		return false;

	const u32 requestedWidth = static_cast<u32>(_width);
	const u32 requestedHeight = static_cast<u32>(_height);
	const u32 copyWidth = std::min<u32>(requestedWidth, m_vk->swapchainExtent.width);
	const u32 copyHeight = std::min<u32>(requestedHeight, m_vk->swapchainExtent.height);
	if (copyWidth == 0 || copyHeight == 0)
		return false;

	const size_t dstRowBytes = static_cast<size_t>(requestedWidth) * 3U;
	const size_t dstBytes = dstRowBytes * static_cast<size_t>(requestedHeight);
	std::memset(_dest, 0, dstBytes);

	u32 imageIndex = m_vk->lastPresentedImageIndex;
	if (imageIndex >= m_vk->swapchainImages.size())
		imageIndex = 0U;
	const VkImage sourceImage = m_vk->swapchainImages[imageIndex];
	if (sourceImage == VK_NULL_HANDLE)
		return false;

	if (vkDeviceWaitIdle(m_vk->device) != VK_SUCCESS)
		return false;

	const VkDeviceSize stagingSize = static_cast<VkDeviceSize>(copyWidth) * static_cast<VkDeviceSize>(copyHeight) * 4U;
	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	VkFence submitFence = VK_NULL_HANDLE;
	void * mapped = nullptr;
	bool success = false;

	do {
		VkBufferCreateInfo bufferCreateInfo{};
		bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferCreateInfo.size = stagingSize;
		bufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(m_vk->device, &bufferCreateInfo, nullptr, &stagingBuffer) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkCreateBuffer failed.");
			break;
		}

		VkMemoryRequirements memoryRequirements{};
		vkGetBufferMemoryRequirements(m_vk->device, stagingBuffer, &memoryRequirements);
		const u32 memoryTypeIndex = findMemoryTypeIndex(
			m_vk->physicalDevice,
			memoryRequirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (memoryTypeIndex == UINT32_MAX) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: no host-visible memory type for staging buffer.");
			break;
		}

		VkMemoryAllocateInfo memoryAllocateInfo{};
		memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		memoryAllocateInfo.allocationSize = memoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = memoryTypeIndex;
		if (vkAllocateMemory(m_vk->device, &memoryAllocateInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkAllocateMemory failed.");
			break;
		}
		if (vkBindBufferMemory(m_vk->device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkBindBufferMemory failed.");
			break;
		}

		VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
		commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandBufferAllocateInfo.commandPool = m_vk->commandPool;
		commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandBufferAllocateInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(m_vk->device, &commandBufferAllocateInfo, &commandBuffer) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkAllocateCommandBuffers failed.");
			break;
		}

		VkCommandBufferBeginInfo commandBufferBeginInfo{};
		commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkBeginCommandBuffer failed.");
			break;
		}

		const VkImageSubresourceRange imageSubresourceRange = {
			VK_IMAGE_ASPECT_COLOR_BIT,
			0, 1,
			0, 1
		};

		VkImageMemoryBarrier toTransferBarrier{};
		toTransferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toTransferBarrier.srcAccessMask = 0;
		toTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		toTransferBarrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		toTransferBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toTransferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toTransferBarrier.image = sourceImage;
		toTransferBarrier.subresourceRange = imageSubresourceRange;
		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &toTransferBarrier);

		VkBufferImageCopy copyRegion{};
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = 0;
		copyRegion.bufferImageHeight = 0;
		copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copyRegion.imageSubresource.mipLevel = 0;
		copyRegion.imageSubresource.baseArrayLayer = 0;
		copyRegion.imageSubresource.layerCount = 1;
		copyRegion.imageOffset = { 0, 0, 0 };
		copyRegion.imageExtent = { copyWidth, copyHeight, 1 };
		vkCmdCopyImageToBuffer(
			commandBuffer,
			sourceImage,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			stagingBuffer,
			1,
			&copyRegion);

		VkImageMemoryBarrier toPresentBarrier{};
		toPresentBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		toPresentBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		toPresentBarrier.dstAccessMask = 0;
		toPresentBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		toPresentBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		toPresentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toPresentBarrier.image = sourceImage;
		toPresentBarrier.subresourceRange = imageSubresourceRange;
		vkCmdPipelineBarrier(
			commandBuffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
			0,
			0, nullptr,
			0, nullptr,
			1, &toPresentBarrier);

		if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkEndCommandBuffer failed.");
			break;
		}

		VkFenceCreateInfo fenceCreateInfo{};
		fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (vkCreateFence(m_vk->device, &fenceCreateInfo, nullptr, &submitFence) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkCreateFence failed.");
			break;
		}

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		if (vkQueueSubmit(m_vk->graphicsQueue, 1, &submitInfo, submitFence) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkQueueSubmit failed.");
			break;
		}
		if (vkWaitForFences(m_vk->device, 1, &submitFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkWaitForFences failed.");
			break;
		}

		if (vkMapMemory(m_vk->device, stagingMemory, 0, stagingSize, 0, &mapped) != VK_SUCCESS || mapped == nullptr) {
			if (debugReadScreen)
				LOG(LOG_WARNING, "VK readScreen2: vkMapMemory failed.");
			break;
		}

		const bool bgraSource = m_vk->swapchainFormat == VK_FORMAT_B8G8R8A8_UNORM
			|| m_vk->swapchainFormat == VK_FORMAT_B8G8R8A8_SRGB;
		const u8 * srcPixels = static_cast<const u8 *>(mapped);
		u8 * dstPixels = static_cast<u8 *>(_dest);
		const size_t srcRowBytes = static_cast<size_t>(copyWidth) * 4U;
		for (u32 y = 0; y < copyHeight; ++y) {
			const u8 * srcRow = srcPixels + static_cast<size_t>(copyHeight - 1U - y) * srcRowBytes;
			u8 * dstRow = dstPixels + static_cast<size_t>(y) * dstRowBytes;
			for (u32 x = 0; x < copyWidth; ++x) {
				const u8 c0 = srcRow[x * 4U + 0U];
				const u8 c1 = srcRow[x * 4U + 1U];
				const u8 c2 = srcRow[x * 4U + 2U];
				if (bgraSource) {
					dstRow[x * 3U + 0U] = c2;
					dstRow[x * 3U + 1U] = c1;
					dstRow[x * 3U + 2U] = c0;
				} else {
					dstRow[x * 3U + 0U] = c0;
					dstRow[x * 3U + 1U] = c1;
					dstRow[x * 3U + 2U] = c2;
				}
			}
		}

		success = true;
	} while (false);

	if (debugReadScreen) {
		LOG(
			LOG_WARNING,
			"VK readScreen2: success=%u copy=%ux%u requested=%dx%d imageIndex=%u format=%d",
			success ? 1U : 0U,
			copyWidth,
			copyHeight,
			_width,
			_height,
			imageIndex,
			static_cast<int>(m_vk->swapchainFormat));
	}

	if (mapped != nullptr)
		vkUnmapMemory(m_vk->device, stagingMemory);
	if (submitFence != VK_NULL_HANDLE)
		vkDestroyFence(m_vk->device, submitFence, nullptr);
	if (commandBuffer != VK_NULL_HANDLE)
		vkFreeCommandBuffers(m_vk->device, m_vk->commandPool, 1, &commandBuffer);
	if (stagingMemory != VK_NULL_HANDLE)
		vkFreeMemory(m_vk->device, stagingMemory, nullptr);
	if (stagingBuffer != VK_NULL_HANDLE)
		vkDestroyBuffer(m_vk->device, stagingBuffer, nullptr);

	return success;
#endif
}

graphics::PixelReadBuffer * ContextImpl::createPixelReadBuffer(size_t _sizeInBytes)
{
	return vulkan::readback::createPixelReadBuffer(
		_sizeInBytes,
		[this](
			s32 _x,
			s32 _y,
			u32 _width,
			u32 _height,
			graphics::Parameter _format,
			graphics::Parameter _type,
			std::vector<u8> & _outData) -> bool {
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
			(void)_x;
			(void)_y;
			(void)_width;
			(void)_height;
			(void)_format;
			(void)_type;
			(void)_outData;
			return false;
#else
			(void)_type;
			if (!m_vk || _width == 0U || _height == 0U)
				return false;

			const s32 clampedX = std::max<s32>(0, _x);
			const s32 clampedY = std::max<s32>(0, _y);
			if (clampedX >= std::numeric_limits<s32>::max() || clampedY >= std::numeric_limits<s32>::max())
				return false;
			const u32 readX = static_cast<u32>(clampedX);
			const u32 readY = static_cast<u32>(clampedY);

			graphics::ObjectHandle readFramebuffer = m_vk->drawRecorder.readFramebufferBinding();
			if (!readFramebuffer.isNotNull())
				readFramebuffer = m_vk->lastNonDefaultReadFramebuffer;
			if (!readFramebuffer.isNotNull())
				readFramebuffer = m_vk->lastColorBlitDrawFramebuffer;
			if (!readFramebuffer.isNotNull())
				readFramebuffer = m_vk->lastNonDefaultDrawFramebuffer;
			if (!readFramebuffer.isNotNull())
				return false;

			const bool depthRead = _format == graphics::colorFormat::DEPTH;
			const u32 readColorAttachmentLimit = m_vk->framebufferStore.getDrawBufferCount(readFramebuffer);
			graphics::BufferAttachmentParam sourceAttachmentType = depthRead
				? graphics::bufferAttachment::DEPTH_ATTACHMENT
				: graphics::bufferAttachment::COLOR_ATTACHMENT0;
			const FramebufferStore::FramebufferAttachment * sourceAttachment = depthRead
				? m_vk->framebufferStore.getAttachment(readFramebuffer, graphics::bufferAttachment::DEPTH_ATTACHMENT)
				: resolveColorAttachment(
					m_vk->framebufferStore,
					readFramebuffer,
					graphics::bufferAttachment::COLOR_ATTACHMENT0,
					&sourceAttachmentType,
					readColorAttachmentLimit);
			if (sourceAttachment == nullptr || !sourceAttachment->textureHandle.isNotNull())
				return false;

			u32 rowBytes = 0U;
			u32 bytesPerPixel = 0U;
			if (!m_vk->textureStore.readTexture(
				sourceAttachment->textureHandle,
				readX,
				readY,
				_width,
				_height,
				_outData,
				rowBytes,
				bytesPerPixel)) {
				return false;
			}
			if (rowBytes == 0U || bytesPerPixel == 0U || (rowBytes % bytesPerPixel) != 0U)
				return false;
			static const bool debugReadback = std::getenv("REALITYVK_VK_DEBUG_READBACK") != nullptr;
			if (debugReadback) {
				static u32 readbackLogCount = 0U;
				static const u32 readbackLogLimit = []() -> u32 {
					const char * env = std::getenv("REALITYVK_VK_DEBUG_READBACK_LIMIT");
					if (env == nullptr || env[0] == '\0')
						return 64U;
					return static_cast<u32>(std::strtoul(env, nullptr, 10));
				}();
				if (readbackLogCount < readbackLogLimit) {
					LOG(
						LOG_WARNING,
						"VK readback debug: kind=pixel fbo=%u attachment=%s texture=%u x=%u y=%u w=%u h=%u rowBytes=%u bpp=%u bytes=%u",
						static_cast<u32>(readFramebuffer),
						bufferAttachmentName(sourceAttachmentType),
						static_cast<u32>(sourceAttachment->textureHandle),
						readX,
						readY,
						_width,
						_height,
						rowBytes,
						bytesPerPixel,
						static_cast<u32>(_outData.size()));
					++readbackLogCount;
				}
			}
			return true;
#endif
		});
}

graphics::ColorBufferReader * ContextImpl::createColorBufferReader(CachedTexture * _pTexture)
{
	return vulkan::readback::createColorBufferReader(
		_pTexture,
		[this](
			graphics::ObjectHandle _textureHandle,
			s32 _x,
			s32 _y,
			u32 _width,
			u32 _height,
			graphics::Parameter _format,
			graphics::Parameter _type,
			std::vector<u8> & _outData,
			u32 & _outStridePixels) -> bool {
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
			(void)_textureHandle;
			(void)_x;
			(void)_y;
			(void)_width;
			(void)_height;
			(void)_format;
			(void)_type;
			(void)_outData;
			_outStridePixels = 0U;
			return false;
#else
			(void)_format;
			(void)_type;
			_outStridePixels = 0U;
			if (!m_vk || !_textureHandle.isNotNull() || _width == 0U || _height == 0U)
				return false;
			const s32 clampedX = std::max<s32>(0, _x);
			const s32 clampedY = std::max<s32>(0, _y);
			const u32 readX = static_cast<u32>(clampedX);
			const u32 readY = static_cast<u32>(clampedY);

			u32 rowBytes = 0U;
			u32 bytesPerPixel = 0U;
			if (!m_vk->textureStore.readTexture(
				_textureHandle,
				readX,
				readY,
				_width,
				_height,
				_outData,
				rowBytes,
				bytesPerPixel)) {
				return false;
			}
			if (rowBytes == 0U || bytesPerPixel == 0U || (rowBytes % bytesPerPixel) != 0U)
				return false;
			static const bool debugReadback = std::getenv("REALITYVK_VK_DEBUG_READBACK") != nullptr;
			if (debugReadback) {
				static u32 readbackLogCount = 0U;
				static const u32 readbackLogLimit = []() -> u32 {
					const char * env = std::getenv("REALITYVK_VK_DEBUG_READBACK_LIMIT");
					if (env == nullptr || env[0] == '\0')
						return 64U;
					return static_cast<u32>(std::strtoul(env, nullptr, 10));
				}();
				if (readbackLogCount < readbackLogLimit) {
					LOG(
						LOG_WARNING,
						"VK readback debug: kind=color texture=%u x=%u y=%u w=%u h=%u rowBytes=%u bpp=%u bytes=%u",
						static_cast<u32>(_textureHandle),
						readX,
						readY,
						_width,
						_height,
						rowBytes,
						bytesPerPixel,
						static_cast<u32>(_outData.size()));
					++readbackLogCount;
				}
			}
			_outStridePixels = rowBytes / bytesPerPixel;
			return _outStridePixels != 0U;
#endif
		});
}

graphics::CombinerProgram * ContextImpl::createCombinerProgram(Combiner & _color, Combiner & _alpha, const CombinerKey & _key)
{
	(void)_color;
	(void)_alpha;
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (m_vk) {
		return m_vk->programLibrary.createCombinerProgram(
			_key,
			[](const CombinerKey & _programKey) -> graphics::CombinerProgram * {
				return vulkan::combiner::createInferredCombinerProgram(_programKey);
			});
	}
#endif
	return vulkan::combiner::createInferredCombinerProgram(_key);
}

bool ContextImpl::saveShadersStorage(const graphics::Combiners & _combiners)
{
	return ShaderKeyStorage::save(_combiners);
}

bool ContextImpl::loadShadersStorage(graphics::Combiners & _combiners)
{
	return ShaderKeyStorage::load(_combiners, [](const CombinerKey & _key) -> graphics::CombinerProgram * {
		return vulkan::combiner::createInferredCombinerProgram(_key);
	});
}

graphics::ShaderProgram * ContextImpl::createDepthFogShader()
{
	return vulkan::special_programs::createDepthFogShader();
}

graphics::TexrectDrawerShaderProgram * ContextImpl::createTexrectDrawerDrawShader()
{
	return vulkan::special_programs::createTexrectDrawerDrawShader();
}

graphics::ShaderProgram * ContextImpl::createTexrectDrawerClearShader()
{
	return vulkan::special_programs::createTexrectDrawerClearShader();
}

graphics::ShaderProgram * ContextImpl::createTexrectUpscaleCopyShader()
{
	return vulkan::special_programs::createTexrectUpscaleCopyShader();
}

graphics::ShaderProgram * ContextImpl::createTexrectColorAndDepthUpscaleCopyShader()
{
	return vulkan::special_programs::createTexrectColorAndDepthUpscaleCopyShader();
}

graphics::ShaderProgram * ContextImpl::createTexrectDownscaleCopyShader()
{
	return vulkan::special_programs::createTexrectDownscaleCopyShader();
}

graphics::ShaderProgram * ContextImpl::createTexrectColorAndDepthDownscaleCopyShader()
{
	return vulkan::special_programs::createTexrectColorAndDepthDownscaleCopyShader();
}

graphics::ShaderProgram * ContextImpl::createGammaCorrectionShader()
{
	return vulkan::special_programs::createGammaCorrectionShader();
}

graphics::ShaderProgram * ContextImpl::createFXAAShader()
{
	return vulkan::special_programs::createFXAAShader();
}

graphics::TextDrawerShaderProgram * ContextImpl::createTextDrawerShader()
{
	return vulkan::special_programs::createTextDrawerShader();
}

void ContextImpl::resetShaderProgram()
{
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (!m_vk)
		return;
	m_vk->drawRecorder.markAllStateDirty();
#endif
}

void ContextImpl::drawTriangles(const graphics::Context::DrawTriangleParameters & _params)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_params;
	return;
#else
	if (!m_vk)
		return;
	if (_params.vertices == nullptr || _params.verticesCount == 0)
		return;

	PrimitiveType primitive = PrimitiveType::Triangles;
	if (!resolvePrimitiveType(_params.mode, primitive))
		return;

	DrawPacket packet{};
	vulkan::packet_builder::initializeTrianglePacket(primitive, _params.combiner, packet);
	assignPacketDebugMetadata(packet, kPacketSourceTriangles, _params.combiner, false);
	vulkan::blendmux::applyStrictBlendMuxPacketState(false, packet);
		if (_params.elements != nullptr && _params.elementsCount > 0) {
			packet.vertices.reserve(_params.elementsCount);
			auto appendIndexedVertex = [&](u32 _vertexIndex) {
				if (_vertexIndex >= _params.verticesCount)
					return;
				vulkan::packet_builder::appendTriangleVertex(_params.vertices[_vertexIndex], _params.flatColors, packet);
			};

			if (_params.elementsType == graphics::datatype::UNSIGNED_SHORT) {
				const u16 * elements = reinterpret_cast<const u16 *>(_params.elements);
				for (u32 i = 0; i < _params.elementsCount; ++i)
					appendIndexedVertex(static_cast<u32>(elements[i]));
			} else if (_params.elementsType == graphics::datatype::UNSIGNED_BYTE) {
				const u8 * elements = reinterpret_cast<const u8 *>(_params.elements);
				for (u32 i = 0; i < _params.elementsCount; ++i)
					appendIndexedVertex(static_cast<u32>(elements[i]));
			} else if (_params.elementsType == graphics::datatype::UNSIGNED_INT) {
				const u32 * elements = reinterpret_cast<const u32 *>(_params.elements);
				for (u32 i = 0; i < _params.elementsCount; ++i)
					appendIndexedVertex(elements[i]);
			} else {
				static bool warnedUnsupportedIndexType = false;
				if (!warnedUnsupportedIndexType) {
					LOG(
						LOG_WARNING,
						"Vulkan draw path does not support index type 0x%x; draw call skipped.",
						static_cast<u32>(_params.elementsType));
					warnedUnsupportedIndexType = true;
				}
				return;
			}
		} else {
			packet.vertices.reserve(_params.verticesCount);
			for (u32 i = 0; i < _params.verticesCount; ++i)
				vulkan::packet_builder::appendTriangleVertex(_params.vertices[i], _params.flatColors, packet);
		}

	if (packet.vertices.empty())
		return;
	vulkan::combiner::applyCombinerSolidColorOverride(_params.combiner, packet);
	attachPacketBindings(m_vk->bindingState, packet);
	normalizePacketTextureCoordinates(m_vk->textureStore, packet);
	if (!isDefaultDrawFramebufferBound()) {
		m_vk->drawRecorder.applyStateToPacket(packet);
		if (!executeOffscreenDrawPacket(packet, m_vk->drawRecorder.drawFramebufferBinding())) {
			offscreenSkippedDrawCalls() += 1U;
			offscreenSkippedVertices() += static_cast<u64>(packet.vertices.size());
			vkFboTrace(
				"op=draw_skip primitive=triangles drawFbo=%u readFbo=%u vertices=%u reason=offscreen_execute_failed",
				static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
				static_cast<u32>(m_vk->drawRecorder.readFramebufferBinding()),
				static_cast<u32>(packet.vertices.size()));
		}
		return;
	}
	if (isVkFboTraceVerboseEnabled()) {
		vkFboTrace(
			"op=draw_enqueue primitive=triangles drawFbo=%u vertices=%u textureMask=0x%x shaderFlags=0x%x",
			static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
			static_cast<u32>(packet.vertices.size()),
			packet.textureSlotMask,
			packet.shaderFlags);
	}
	vulkan::packet_normalize::normalizePacketPositions(packet);
	m_vk->drawRecorder.pushPacket(std::move(packet));
#endif
}

void ContextImpl::drawRects(const graphics::Context::DrawRectParameters & _params)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_params;
	return;
#else
	if (!m_vk)
		return;
	if (_params.vertices == nullptr || _params.verticesCount == 0)
		return;

	PrimitiveType primitive = PrimitiveType::Triangles;
	if (!resolvePrimitiveType(_params.mode, primitive))
		return;

	DrawPacket packet{};
	vulkan::packet_builder::initializeRectPacket(primitive, _params.combiner, _params.texrect, packet);
	assignPacketDebugMetadata(packet, kPacketSourceRects, _params.combiner, _params.texrect);
	const bool isClearRectPass = vulkan::special_programs::isTexrectClearProgram(_params.combiner);
	vulkan::special_programs::applyRectSpecialProgramState(_params.combiner, packet);
	if ((packet.shaderFlags & (vulkan::draw_shader_flags::kSpecialTexrectDraw
		| vulkan::draw_shader_flags::kSpecialDepthFromTexture1
		| vulkan::draw_shader_flags::kSpecialGammaCorrection
		| vulkan::draw_shader_flags::kSpecialFXAA
		| vulkan::draw_shader_flags::kSpecialDepthFog
		| vulkan::draw_shader_flags::kSpecialTextDraw)) != 0U) {
		packet.shaderFlags &= ~vulkan::draw_shader_flags::kSpecialStrictBlendMux;
		packet.blendMux1Packed = 0U;
		packet.blendMux2Packed = 0U;
		packet.blendParamsPacked = 0U;
	} else {
		vulkan::blendmux::applyStrictBlendMuxPacketState(_params.texrect, packet);
	}
	packet.vertices.reserve(_params.verticesCount);
	for (u32 i = 0; i < _params.verticesCount; ++i)
		vulkan::packet_builder::appendRectVertex(_params.vertices[i], packet);
	if (isClearRectPass)
		vulkan::packet_builder::overwritePacketVertexColor(packet, 0.0f, 0.0f, 0.0f, 0.0f);

	if (packet.vertices.empty())
		return;
	vulkan::combiner::applyCombinerSolidColorOverride(_params.combiner, packet);
	attachPacketBindings(m_vk->bindingState, packet);
	normalizePacketTextureCoordinates(m_vk->textureStore, packet);
	if (!isDefaultDrawFramebufferBound()) {
		m_vk->drawRecorder.applyStateToPacket(packet);
		if (!executeOffscreenDrawPacket(packet, m_vk->drawRecorder.drawFramebufferBinding())) {
			offscreenSkippedDrawCalls() += 1U;
			offscreenSkippedVertices() += static_cast<u64>(packet.vertices.size());
			vkFboTrace(
				"op=draw_skip primitive=rects drawFbo=%u readFbo=%u vertices=%u reason=offscreen_execute_failed",
				static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
				static_cast<u32>(m_vk->drawRecorder.readFramebufferBinding()),
				static_cast<u32>(packet.vertices.size()));
		}
		return;
	}
	if (isVkFboTraceVerboseEnabled()) {
		vkFboTrace(
			"op=draw_enqueue primitive=rects drawFbo=%u vertices=%u textureMask=0x%x shaderFlags=0x%x",
			static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
			static_cast<u32>(packet.vertices.size()),
			packet.textureSlotMask,
			packet.shaderFlags);
	}
	vulkan::packet_normalize::normalizePacketPositions(packet);
	m_vk->drawRecorder.pushPacket(std::move(packet));
#endif
}

void ContextImpl::drawLine(f32 _width, SPVertex * _vertices)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_width;
	(void)_vertices;
	return;
#else
	if (!m_vk)
		return;
	if (_vertices == nullptr)
		return;

	DrawPacket packet{};
	vulkan::packet_builder::initializeLinePacket(_width, packet);
	assignPacketDebugMetadata(packet, kPacketSourceLines, nullptr, false);
	packet.vertices.reserve(2);
	for (u32 i = 0; i < 2; ++i)
		vulkan::packet_builder::appendLineVertex(_vertices[i], packet);

	attachPacketBindings(m_vk->bindingState, packet);
	normalizePacketTextureCoordinates(m_vk->textureStore, packet);
	if (!isDefaultDrawFramebufferBound()) {
		m_vk->drawRecorder.applyStateToPacket(packet, draw_dirty::kLineWidth);
		if (!executeOffscreenDrawPacket(packet, m_vk->drawRecorder.drawFramebufferBinding())) {
			offscreenSkippedDrawCalls() += 1U;
			offscreenSkippedVertices() += static_cast<u64>(packet.vertices.size());
			vkFboTrace(
				"op=draw_skip primitive=lines drawFbo=%u readFbo=%u vertices=%u reason=offscreen_execute_failed",
				static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
				static_cast<u32>(m_vk->drawRecorder.readFramebufferBinding()),
				static_cast<u32>(packet.vertices.size()));
		}
		return;
	}
	if (isVkFboTraceVerboseEnabled()) {
		vkFboTrace(
			"op=draw_enqueue primitive=lines drawFbo=%u vertices=%u textureMask=0x%x shaderFlags=0x%x",
			static_cast<u32>(m_vk->drawRecorder.drawFramebufferBinding()),
			static_cast<u32>(packet.vertices.size()),
			packet.textureSlotMask,
			packet.shaderFlags);
	}
	vulkan::packet_normalize::normalizePacketPositions(packet);
	m_vk->drawRecorder.pushPacket(std::move(packet), draw_dirty::kLineWidth);
#endif
}

bool ContextImpl::executeOffscreenDrawPacket(const DrawPacket & _packet, graphics::ObjectHandle _drawFramebuffer)
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	(void)_packet;
	(void)_drawFramebuffer;
	return false;
#else
	if (!m_vk || m_vk->device == VK_NULL_HANDLE || m_vk->commandPool == VK_NULL_HANDLE || m_vk->graphicsQueue == VK_NULL_HANDLE)
		return false;
	if (!_drawFramebuffer.isNotNull())
		return false;
	if (_packet.vertices.empty())
		return true;

	const u32 drawColorAttachmentLimit = m_vk->framebufferStore.getDrawBufferCount(_drawFramebuffer);
	graphics::BufferAttachmentParam colorAttachmentType = graphics::bufferAttachment::COLOR_ATTACHMENT0;
	const FramebufferStore::FramebufferAttachment * colorAttachment = resolveColorAttachment(
		m_vk->framebufferStore,
		_drawFramebuffer,
		graphics::bufferAttachment::COLOR_ATTACHMENT0,
		&colorAttachmentType,
		drawColorAttachmentLimit);
	if (colorAttachment == nullptr || !colorAttachment->textureHandle.isNotNull()) {
		vkFboTrace(
			"op=draw_offscreen_fail drawFbo=%u reason=no_color_attachment",
			static_cast<u32>(_drawFramebuffer));
		return false;
	}

	TextureStore::TextureResource * colorTexture = m_vk->textureStore.getTextureMutable(colorAttachment->textureHandle);
	if (colorTexture == nullptr
		|| colorTexture->image == VK_NULL_HANDLE
		|| colorTexture->imageView == VK_NULL_HANDLE
		|| colorTexture->width == 0
		|| colorTexture->height == 0
		|| colorTexture->vkFormat == VK_FORMAT_UNDEFINED) {
		vkFboTrace(
			"op=draw_offscreen_fail drawFbo=%u reason=invalid_color_texture attachment=%s texture=%u",
			static_cast<u32>(_drawFramebuffer),
			bufferAttachmentName(colorAttachmentType),
			static_cast<u32>(colorAttachment->textureHandle));
		return false;
	}
	if (colorTexture->msaaLevel != 0U) {
		static bool warnedColorMsaaMetadata = false;
		if (!warnedColorMsaaMetadata) {
			LOG(
				LOG_WARNING,
				"VK offscreen draw: color texture msaaLevel metadata=%u on texture=%u; continuing with single-sample path.",
				colorTexture->msaaLevel,
				static_cast<u32>(colorAttachment->textureHandle));
			warnedColorMsaaMetadata = true;
		}
	}

	TextureStore::TextureResource * depthTexture = nullptr;
	const FramebufferStore::FramebufferAttachment * depthAttachment = m_vk->framebufferStore.getAttachment(
		_drawFramebuffer,
		graphics::bufferAttachment::DEPTH_ATTACHMENT);
	if (depthAttachment != nullptr && depthAttachment->textureHandle.isNotNull()) {
		depthTexture = m_vk->textureStore.getTextureMutable(depthAttachment->textureHandle);
		if (depthTexture != nullptr
			&& (depthTexture->image == VK_NULL_HANDLE
				|| depthTexture->imageView == VK_NULL_HANDLE
				|| depthTexture->width == 0U
				|| depthTexture->height == 0U)) {
			vkFboTrace(
				"op=draw_offscreen depth_detach drawFbo=%u depthTexture=%u reason=invalid_depth_texture colorSize=%ux%u",
				static_cast<u32>(_drawFramebuffer),
				static_cast<u32>(depthAttachment->textureHandle),
				colorTexture->width,
				colorTexture->height);
			depthTexture = nullptr;
		} else if (depthTexture != nullptr && depthTexture->msaaLevel != 0U) {
			static bool warnedDepthMsaaMetadata = false;
			if (!warnedDepthMsaaMetadata) {
				LOG(
					LOG_WARNING,
					"VK offscreen draw: depth texture msaaLevel metadata=%u on texture=%u; continuing with single-sample path.",
					depthTexture->msaaLevel,
					static_cast<u32>(depthAttachment->textureHandle));
				warnedDepthMsaaMetadata = true;
			}
		}
	}

	u32 renderWidth = colorTexture->width;
	u32 renderHeight = colorTexture->height;
	if (depthTexture != nullptr) {
		const bool depthSizeMismatch = depthTexture->width != colorTexture->width
			|| depthTexture->height != colorTexture->height;
		if (depthSizeMismatch) {
			const u32 clampedWidth = std::min(colorTexture->width, depthTexture->width);
			const u32 clampedHeight = std::min(colorTexture->height, depthTexture->height);
			const bool needsDepth = _packet.state.depth.testEnabled || _packet.state.depth.writeEnabled;
			if (!needsDepth || clampedWidth == 0U || clampedHeight == 0U) {
				vkFboTrace(
					"op=draw_offscreen depth_detach drawFbo=%u depthTexture=%u reason=size_mismatch_depth_unused_or_empty colorSize=%ux%u depthSize=%ux%u",
					static_cast<u32>(_drawFramebuffer),
					static_cast<u32>(depthAttachment->textureHandle),
					colorTexture->width,
					colorTexture->height,
					depthTexture->width,
					depthTexture->height);
				depthTexture = nullptr;
			} else {
				vkFboTrace(
					"op=draw_offscreen extent_clamp drawFbo=%u colorSize=%ux%u depthSize=%ux%u clamped=%ux%u",
					static_cast<u32>(_drawFramebuffer),
					colorTexture->width,
					colorTexture->height,
					depthTexture->width,
					depthTexture->height,
					clampedWidth,
					clampedHeight);
				renderWidth = clampedWidth;
				renderHeight = clampedHeight;
			}
		}
	}
	if (renderWidth == 0U || renderHeight == 0U) {
		vkFboTrace(
			"op=draw_offscreen_fail drawFbo=%u reason=zero_render_extent colorSize=%ux%u",
			static_cast<u32>(_drawFramebuffer),
			colorTexture->width,
			colorTexture->height);
		return false;
	}
	const VkExtent2D renderExtent = { renderWidth, renderHeight };
	VkRenderPass renderPass = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkBuffer vertexBuffer = VK_NULL_HANDLE;
	VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
	VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
	PipelineCache offscreenPipelineCache;
	bool success = false;
	bool commandBufferAllocated = false;

	auto cleanup = [&]() {
		offscreenPipelineCache.destroy();
		if (commandBufferAllocated && commandBuffer != VK_NULL_HANDLE)
			vkFreeCommandBuffers(m_vk->device, m_vk->commandPool, 1, &commandBuffer);
		if (vertexBuffer != VK_NULL_HANDLE)
			vkDestroyBuffer(m_vk->device, vertexBuffer, nullptr);
		if (vertexMemory != VK_NULL_HANDLE)
			vkFreeMemory(m_vk->device, vertexMemory, nullptr);
		if (framebuffer != VK_NULL_HANDLE)
			vkDestroyFramebuffer(m_vk->device, framebuffer, nullptr);
		if (renderPass != VK_NULL_HANDLE)
			vkDestroyRenderPass(m_vk->device, renderPass, nullptr);
	};

	do {
		VkAttachmentDescription attachments[2]{};
		u32 attachmentCount = 0U;
		VkClearValue clearValues[2]{};

		const bool colorWasUndefined = colorTexture->imageLayout == VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[attachmentCount].flags = 0;
		attachments[attachmentCount].format = colorTexture->vkFormat;
		attachments[attachmentCount].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[attachmentCount].loadOp = colorWasUndefined ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[attachmentCount].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[attachmentCount].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[attachmentCount].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[attachmentCount].initialLayout = colorWasUndefined
			? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
			: colorTexture->imageLayout;
		attachments[attachmentCount].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		clearValues[attachmentCount].color.float32[0] = 0.0f;
		clearValues[attachmentCount].color.float32[1] = 0.0f;
		clearValues[attachmentCount].color.float32[2] = 0.0f;
		clearValues[attachmentCount].color.float32[3] = 0.0f;
		VkAttachmentReference colorAttachmentRef{};
		colorAttachmentRef.attachment = attachmentCount;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		++attachmentCount;

		VkAttachmentReference depthAttachmentRef{};
		const bool hasDepth = depthTexture != nullptr;
		if (hasDepth) {
			const bool depthWasUndefined = depthTexture->imageLayout == VK_IMAGE_LAYOUT_UNDEFINED;
			attachments[attachmentCount].flags = 0;
			attachments[attachmentCount].format = depthTexture->vkFormat;
			attachments[attachmentCount].samples = VK_SAMPLE_COUNT_1_BIT;
			attachments[attachmentCount].loadOp = depthWasUndefined ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
			attachments[attachmentCount].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachments[attachmentCount].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachments[attachmentCount].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			attachments[attachmentCount].initialLayout = depthWasUndefined
				? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
				: depthTexture->imageLayout;
			attachments[attachmentCount].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			clearValues[attachmentCount].depthStencil.depth = 1.0f;
			clearValues[attachmentCount].depthStencil.stencil = 0U;
			depthAttachmentRef.attachment = attachmentCount;
			depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			++attachmentCount;
		}

		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorAttachmentRef;
		subpass.pDepthStencilAttachment = hasDepth ? &depthAttachmentRef : nullptr;

		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
			| VK_PIPELINE_STAGE_TRANSFER_BIT
			| VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
			| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
			| VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
			| VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT
			| VK_ACCESS_TRANSFER_READ_BIT
			| VK_ACCESS_TRANSFER_WRITE_BIT
			| VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
			| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
			| VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
			| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
			| VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo renderPassCreateInfo{};
		renderPassCreateInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassCreateInfo.attachmentCount = attachmentCount;
		renderPassCreateInfo.pAttachments = attachments;
		renderPassCreateInfo.subpassCount = 1;
		renderPassCreateInfo.pSubpasses = &subpass;
		renderPassCreateInfo.dependencyCount = 1;
		renderPassCreateInfo.pDependencies = &dependency;
		if (vkCreateRenderPass(m_vk->device, &renderPassCreateInfo, nullptr, &renderPass) != VK_SUCCESS)
			break;

		VkImageView framebufferAttachments[2] = {
			colorTexture->imageView,
			hasDepth ? depthTexture->imageView : VK_NULL_HANDLE
		};
		VkFramebufferCreateInfo framebufferCreateInfo{};
		framebufferCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebufferCreateInfo.renderPass = renderPass;
		framebufferCreateInfo.attachmentCount = hasDepth ? 2U : 1U;
		framebufferCreateInfo.pAttachments = framebufferAttachments;
		framebufferCreateInfo.width = renderExtent.width;
		framebufferCreateInfo.height = renderExtent.height;
		framebufferCreateInfo.layers = 1;
		if (vkCreateFramebuffer(m_vk->device, &framebufferCreateInfo, nullptr, &framebuffer) != VK_SUCCESS)
			break;

		if (!offscreenPipelineCache.init(
			m_vk->device,
			renderPass,
			m_vk->programLibrary.getBasicColorPipelineLayout(),
			m_vk->programLibrary.getBasicColorVertexShader(),
			m_vk->programLibrary.getBasicColorFragmentShader(),
			m_vk->programLibrary.getBasicTexturedVertexShader(),
			m_vk->programLibrary.getBasicTexturedFragmentShader())) {
			break;
		}

			DrawPacket normalizePacket = _packet;
			normalizePacket.state.raster.viewportX = 0;
				normalizePacket.state.raster.viewportY = 0;
				normalizePacket.state.raster.viewportWidth = static_cast<s32>(renderExtent.width);
				normalizePacket.state.raster.viewportHeight = static_cast<s32>(renderExtent.height);
				normalizePacket.state.raster.viewportValid = true;
			static const bool forceOffscreenRasterRectTransform = std::getenv("REALITYVK_VK_FORCE_OFFSCREEN_RASTER_RECT_TRANSFORM") != nullptr;
				normalizePacket.forceRasterRectTransform = forceOffscreenRasterRectTransform
					&& normalizePacket.transformMode == vulkan::VertexTransformMode::Rect;
			std::vector<DrawVertex> normalizedVertices;
			normalizedVertices.reserve(_packet.vertices.size());
			for (const DrawVertex & vertex : _packet.vertices)
				normalizedVertices.push_back(vulkan::packet_normalize::normalizePacketVertex(normalizePacket, vertex));
		const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(normalizedVertices.size() * sizeof(DrawVertex));
		if (vertexBytes == 0)
			break;

		VkBufferCreateInfo vertexBufferCreateInfo{};
		vertexBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		vertexBufferCreateInfo.size = vertexBytes;
		vertexBufferCreateInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		vertexBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateBuffer(m_vk->device, &vertexBufferCreateInfo, nullptr, &vertexBuffer) != VK_SUCCESS)
			break;

		VkMemoryRequirements vertexMemoryRequirements{};
		vkGetBufferMemoryRequirements(m_vk->device, vertexBuffer, &vertexMemoryRequirements);
		const u32 memoryTypeIndex = findMemoryTypeIndex(
			m_vk->physicalDevice,
			vertexMemoryRequirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (memoryTypeIndex == UINT32_MAX)
			break;

		VkMemoryAllocateInfo memoryAllocateInfo{};
		memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		memoryAllocateInfo.allocationSize = vertexMemoryRequirements.size;
		memoryAllocateInfo.memoryTypeIndex = memoryTypeIndex;
		if (vkAllocateMemory(m_vk->device, &memoryAllocateInfo, nullptr, &vertexMemory) != VK_SUCCESS)
			break;
		if (vkBindBufferMemory(m_vk->device, vertexBuffer, vertexMemory, 0) != VK_SUCCESS)
			break;

		void * mapped = nullptr;
		if (vkMapMemory(m_vk->device, vertexMemory, 0, vertexBytes, 0, &mapped) != VK_SUCCESS || mapped == nullptr)
			break;
		std::memcpy(mapped, normalizedVertices.data(), static_cast<size_t>(vertexBytes));
		vkUnmapMemory(m_vk->device, vertexMemory);

		VkCommandBufferAllocateInfo commandAllocateInfo{};
		commandAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		commandAllocateInfo.commandPool = m_vk->commandPool;
		commandAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		commandAllocateInfo.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(m_vk->device, &commandAllocateInfo, &commandBuffer) != VK_SUCCESS)
			break;
		commandBufferAllocated = true;

		VkCommandBufferBeginInfo commandBeginInfo{};
		commandBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		commandBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(commandBuffer, &commandBeginInfo) != VK_SUCCESS)
			break;

		VkRenderPassBeginInfo renderPassBeginInfo{};
		renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassBeginInfo.renderPass = renderPass;
		renderPassBeginInfo.framebuffer = framebuffer;
		renderPassBeginInfo.renderArea.offset = { 0, 0 };
		renderPassBeginInfo.renderArea.extent = renderExtent;
		renderPassBeginInfo.clearValueCount = attachmentCount;
		renderPassBeginInfo.pClearValues = clearValues;
		vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

		m_vk->descriptorBinder.beginFrame(m_vk->frameSyncIndex);
		DrawPacket packetCopy = _packet;
		static const bool disableStrictOffscreen = std::getenv("REALITYVK_VK_STRICT_OFFSCREEN_DISABLE") != nullptr;
		if (disableStrictOffscreen) {
			packetCopy.shaderFlags &= ~vulkan::draw_shader_flags::kSpecialStrictBlendMux;
			packetCopy.blendMux1Packed = 0U;
			packetCopy.blendMux2Packed = 0U;
			packetCopy.blendParamsPacked = 0U;
		}
		packetCopy.vertices = std::move(normalizedVertices);
		static const bool debugFlipRtTexrectY = std::getenv("REALITYVK_VK_DEBUG_FLIP_RT_TEXRECT_Y") != nullptr;
		if (debugFlipRtTexrectY
			&& packetCopy.debugTexrect
			&& !packetCopy.vertices.empty()) {
			bool sampleRtTexture = false;
			for (u32 slot = 0U; slot < 2U; ++slot) {
				if ((packetCopy.textureSlotMask & (1U << slot)) == 0U)
					continue;
				const TextureStore::TextureResource * texture = m_vk->textureStore.getTexture(packetCopy.textureSlots[slot].texture);
				if (texture != nullptr && texture->renderTargetWritten) {
					sampleRtTexture = true;
					break;
				}
			}
			if (sampleRtTexture) {
				for (DrawVertex & vertex : packetCopy.vertices) {
					vertex.t0 = 1.0f - vertex.t0;
					vertex.t1 = 1.0f - vertex.t1;
				}
			}
		}
		static const bool debugFlipOffscreenTexrectY = std::getenv("REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_TEXRECT_Y") != nullptr;
		static const u32 debugFlipOffscreenTexrectHandle = []() -> u32 {
			const char * env = std::getenv("REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_TEXRECT_HANDLE");
			if (env == nullptr || env[0] == '\0')
				return 0U;
			return static_cast<u32>(std::strtoul(env, nullptr, 10));
		}();
		if (debugFlipOffscreenTexrectY
			&& packetCopy.debugSource == kPacketSourceRects
			&& packetCopy.debugTexrect
			&& (packetCopy.textureSlotMask & (1U << 0)) != 0U
			&& !packetCopy.vertices.empty()) {
			bool handleMatch = debugFlipOffscreenTexrectHandle == 0U;
			if (!handleMatch) {
				for (u32 slot = 0U; slot < 2U; ++slot) {
					if ((packetCopy.textureSlotMask & (1U << slot)) == 0U)
						continue;
					if (static_cast<u32>(packetCopy.textureSlots[slot].texture) == debugFlipOffscreenTexrectHandle) {
						handleMatch = true;
						break;
					}
				}
			}
			if (handleMatch) {
				for (DrawVertex & vertex : packetCopy.vertices) {
					vertex.t0 = 1.0f - vertex.t0;
					vertex.t1 = 1.0f - vertex.t1;
				}
			}
		}
		static const bool debugFlipOffscreenTexrectPosY = std::getenv("REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_TEXRECT_POS_Y") != nullptr;
		if (debugFlipOffscreenTexrectPosY
			&& packetCopy.debugSource == kPacketSourceRects
			&& packetCopy.debugTexrect
			&& !packetCopy.vertices.empty()) {
			for (DrawVertex & vertex : packetCopy.vertices)
				vertex.y = -vertex.y;
		}
		static const bool debugFlipOffscreenFillRectPosY = std::getenv("REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_FILLRECT_POS_Y") != nullptr;
		if (debugFlipOffscreenFillRectPosY
			&& packetCopy.debugSource == kPacketSourceRects
			&& !packetCopy.debugTexrect
			&& !packetCopy.vertices.empty()) {
			for (DrawVertex & vertex : packetCopy.vertices)
				vertex.y = -vertex.y;
		}
		static const bool debugFlipOffscreenTriangleY = std::getenv("REALITYVK_VK_DEBUG_FLIP_OFFSCREEN_TRIANGLE_Y") != nullptr;
		if (debugFlipOffscreenTriangleY
			&& packetCopy.transformMode == vulkan::VertexTransformMode::Triangle
			&& !packetCopy.vertices.empty()) {
			for (DrawVertex & vertex : packetCopy.vertices)
				vertex.y = -vertex.y;
		}
		static const bool debugPaintOffscreenTriangles = std::getenv("REALITYVK_VK_DEBUG_PAINT_OFFSCREEN_TRIANGLES") != nullptr;
		if (debugPaintOffscreenTriangles
			&& packetCopy.transformMode == vulkan::VertexTransformMode::Triangle
			&& !packetCopy.vertices.empty()) {
			packetCopy.textureSlotMask = 0U;
			packetCopy.shaderFlags = vulkan::draw_shader_flags::kShade;
			packetCopy.blendMux1Packed = 0U;
			packetCopy.blendMux2Packed = 0U;
			packetCopy.blendParamsPacked = 0U;
			for (DrawVertex & vertex : packetCopy.vertices) {
				vertex.r = 0.0f;
				vertex.g = 1.0f;
				vertex.b = 0.0f;
				vertex.a = 1.0f;
			}
		}
		static const bool debugPaintOffscreenTexrects = std::getenv("REALITYVK_VK_DEBUG_PAINT_OFFSCREEN_TEXRECTS") != nullptr;
		if (debugPaintOffscreenTexrects
			&& packetCopy.transformMode == vulkan::VertexTransformMode::Rect
			&& packetCopy.debugTexrect
			&& !packetCopy.vertices.empty()) {
			packetCopy.textureSlotMask = 0U;
			packetCopy.shaderFlags = vulkan::draw_shader_flags::kShade;
			packetCopy.blendMux1Packed = 0U;
			packetCopy.blendMux2Packed = 0U;
			packetCopy.blendParamsPacked = 0U;
			for (DrawVertex & vertex : packetCopy.vertices) {
				vertex.r = 1.0f;
				vertex.g = 0.0f;
				vertex.b = 1.0f;
				vertex.a = 1.0f;
			}
		}
			packetCopy.state.raster.viewportX = 0;
			packetCopy.state.raster.viewportY = 0;
			packetCopy.state.raster.viewportWidth = static_cast<s32>(renderExtent.width);
			packetCopy.state.raster.viewportHeight = static_cast<s32>(renderExtent.height);
			packetCopy.state.raster.viewportValid = true;
			packetCopy.dirtyMask |= draw_dirty::kViewport;
		static const bool debugOffscreen = std::getenv("REALITYVK_VK_DEBUG_OFFSCREEN") != nullptr;
		static const u32 debugTraceRtHandle = []() -> u32 {
			const char * env = std::getenv("REALITYVK_VK_DEBUG_TRACE_RT_HANDLE");
			if (env == nullptr || env[0] == '\0')
				return 0U;
			return static_cast<u32>(std::strtoul(env, nullptr, 10));
		}();
		static u32 debugTraceRtLogCount = 0U;
		if (debugOffscreen && !packetCopy.vertices.empty()) {
			static const u32 offscreenLogLimit = []() -> u32 {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_OFFSCREEN_LIMIT");
				if (env == nullptr || env[0] == '\0')
					return 96U;
				return static_cast<u32>(std::strtoul(env, nullptr, 10));
			}();
			static u32 offscreenLogCount = 0U;
			if (offscreenLogCount < offscreenLogLimit) {
				f32 preMinX = _packet.vertices[0].x;
				f32 preMinY = _packet.vertices[0].y;
				f32 preMaxX = preMinX;
				f32 preMaxY = preMinY;
				f32 minX = packetCopy.vertices[0].x;
				f32 minY = packetCopy.vertices[0].y;
				f32 maxX = minX;
				f32 maxY = minY;
				f32 minS0 = packetCopy.vertices[0].s0;
				f32 minT0 = packetCopy.vertices[0].t0;
				f32 maxS0 = minS0;
				f32 maxT0 = minT0;
				f32 minZ = packetCopy.vertices[0].z;
				f32 maxZ = minZ;
				f32 minW = packetCopy.vertices[0].w;
				f32 maxW = minW;
				f32 minR = packetCopy.vertices[0].r;
				f32 minG = packetCopy.vertices[0].g;
				f32 minB = packetCopy.vertices[0].b;
				f32 minA = packetCopy.vertices[0].a;
				f32 maxR = minR;
				f32 maxG = minG;
				f32 maxB = minB;
				f32 maxA = minA;
				for (const DrawVertex & vertex : _packet.vertices) {
					preMinX = std::min(preMinX, vertex.x);
					preMinY = std::min(preMinY, vertex.y);
					preMaxX = std::max(preMaxX, vertex.x);
					preMaxY = std::max(preMaxY, vertex.y);
				}
				for (const DrawVertex & vertex : packetCopy.vertices) {
					minX = std::min(minX, vertex.x);
					minY = std::min(minY, vertex.y);
					maxX = std::max(maxX, vertex.x);
					maxY = std::max(maxY, vertex.y);
					minS0 = std::min(minS0, vertex.s0);
					minT0 = std::min(minT0, vertex.t0);
					maxS0 = std::max(maxS0, vertex.s0);
					maxT0 = std::max(maxT0, vertex.t0);
					minZ = std::min(minZ, vertex.z);
					maxZ = std::max(maxZ, vertex.z);
					minW = std::min(minW, vertex.w);
					maxW = std::max(maxW, vertex.w);
					minR = std::min(minR, vertex.r);
					minG = std::min(minG, vertex.g);
					minB = std::min(minB, vertex.b);
					minA = std::min(minA, vertex.a);
					maxR = std::max(maxR, vertex.r);
					maxG = std::max(maxG, vertex.g);
					maxB = std::max(maxB, vertex.b);
					maxA = std::max(maxA, vertex.a);
				}
					LOG(
						LOG_WARNING,
						"VK offscreen debug: id=%llu src=%s texrect=%u mux=0x%016llx cycle=%u fbo=%u extent=%ux%u mode=%s vertices=%u shaderFlags=0x%08x texMask=0x%08x blend=[en=%u src=%u,%u dst=%u,%u] depth=[test=%u write=%u cmp=%u] prePos=[%.3f,%.3f]-[%.3f,%.3f] pos=[%.3f,%.3f]-[%.3f,%.3f] z=[%.3f,%.3f] w=[%.3f,%.3f] color=[%.3f,%.3f,%.3f,%.3f]-[%.3f,%.3f,%.3f,%.3f] tex0=[%.3f,%.3f]-[%.3f,%.3f] srcViewport=%d,%d,%d,%d srcViewportValid=%u",
						static_cast<unsigned long long>(packetCopy.debugPacketId),
						packetSourceName(packetCopy.debugSource),
						packetCopy.debugTexrect ? 1U : 0U,
						static_cast<unsigned long long>(packetCopy.debugCombinerMux),
						packetCopy.debugCombinerCycleType,
						static_cast<u32>(_drawFramebuffer),
						renderExtent.width,
						renderExtent.height,
					transformModeName(packetCopy.transformMode),
						static_cast<u32>(packetCopy.vertices.size()),
						packetCopy.shaderFlags,
						packetCopy.textureSlotMask,
						packetCopy.state.blend.enabled ? 1U : 0U,
						static_cast<u32>(packetCopy.state.blend.srcColor),
						static_cast<u32>(packetCopy.state.blend.srcAlpha),
						static_cast<u32>(packetCopy.state.blend.dstColor),
						static_cast<u32>(packetCopy.state.blend.dstAlpha),
						packetCopy.state.depth.testEnabled ? 1U : 0U,
						packetCopy.state.depth.writeEnabled ? 1U : 0U,
						static_cast<u32>(packetCopy.state.depth.compare),
					preMinX,
					preMinY,
					preMaxX,
					preMaxY,
					minX,
					minY,
					maxX,
					maxY,
					minZ,
						maxZ,
						minW,
						maxW,
						minR,
						minG,
						minB,
						minA,
						maxR,
						maxG,
						maxB,
						maxA,
						minS0,
						minT0,
						maxS0,
						maxT0,
					_packet.state.raster.viewportX,
					_packet.state.raster.viewportY,
					_packet.state.raster.viewportWidth,
					_packet.state.raster.viewportHeight,
					_packet.state.raster.viewportValid ? 1U : 0U);
				++offscreenLogCount;
			}
		}
		if (debugTraceRtHandle != 0U
			&& debugTraceRtLogCount < 256U
			&& static_cast<u32>(colorAttachment->textureHandle) == debugTraceRtHandle) {
			f32 minX = 0.0f;
			f32 minY = 0.0f;
			f32 maxX = 0.0f;
			f32 maxY = 0.0f;
			f32 minS0 = 0.0f;
			f32 maxS0 = 0.0f;
			f32 minT0 = 0.0f;
			f32 maxT0 = 0.0f;
			f32 minS1 = 0.0f;
			f32 maxS1 = 0.0f;
			f32 minT1 = 0.0f;
			f32 maxT1 = 0.0f;
			if (!packetCopy.vertices.empty()) {
				minX = maxX = packetCopy.vertices[0].x;
				minY = maxY = packetCopy.vertices[0].y;
				minS0 = maxS0 = packetCopy.vertices[0].s0;
				minT0 = maxT0 = packetCopy.vertices[0].t0;
				minS1 = maxS1 = packetCopy.vertices[0].s1;
				minT1 = maxT1 = packetCopy.vertices[0].t1;
				for (const DrawVertex & vertex : packetCopy.vertices) {
					minX = std::min(minX, vertex.x);
					maxX = std::max(maxX, vertex.x);
					minY = std::min(minY, vertex.y);
					maxY = std::max(maxY, vertex.y);
					minS0 = std::min(minS0, vertex.s0);
					maxS0 = std::max(maxS0, vertex.s0);
					minT0 = std::min(minT0, vertex.t0);
					maxT0 = std::max(maxT0, vertex.t0);
					minS1 = std::min(minS1, vertex.s1);
					maxS1 = std::max(maxS1, vertex.s1);
					minT1 = std::min(minT1, vertex.t1);
					maxT1 = std::max(maxT1, vertex.t1);
				}
			}
			const DrawVertex * v0 = packetCopy.vertices.size() > 0 ? &packetCopy.vertices[0] : nullptr;
			const DrawVertex * v1 = packetCopy.vertices.size() > 1 ? &packetCopy.vertices[1] : nullptr;
			const DrawVertex * v2 = packetCopy.vertices.size() > 2 ? &packetCopy.vertices[2] : nullptr;
			const DrawVertex * v3 = packetCopy.vertices.size() > 3 ? &packetCopy.vertices[3] : nullptr;
			u32 sampledHandle0 = 0U;
			u32 sampledHandle1 = 0U;
			u32 sampledHandle0Rt = 0U;
			u32 sampledHandle1Rt = 0U;
			if ((packetCopy.textureSlotMask & (1U << 0U)) != 0U) {
				sampledHandle0 = static_cast<u32>(packetCopy.textureSlots[0U].texture);
				const TextureStore::TextureResource * sampled0 = m_vk->textureStore.getTexture(packetCopy.textureSlots[0U].texture);
				sampledHandle0Rt = sampled0 != nullptr && sampled0->renderTargetWritten ? 1U : 0U;
			}
			if ((packetCopy.textureSlotMask & (1U << 1U)) != 0U) {
				sampledHandle1 = static_cast<u32>(packetCopy.textureSlots[1U].texture);
				const TextureStore::TextureResource * sampled1 = m_vk->textureStore.getTexture(packetCopy.textureSlots[1U].texture);
				sampledHandle1Rt = sampled1 != nullptr && sampled1->renderTargetWritten ? 1U : 0U;
			}
			LOG(
				LOG_WARNING,
				"VK RT trace(write): handle=%u fbo=%u packetId=%llu src=%s texrect=%u mode=%s mux=0x%016llx cycle=%u vertices=%u sample0=%u(rt=%u) sample1=%u(rt=%u) pos=[%.6f,%.6f]-[%.6f,%.6f] tex0=[%.6f,%.6f]-[%.6f,%.6f] tex1=[%.6f,%.6f]-[%.6f,%.6f] v0=[%.6f,%.6f,%.6f,%.6f] v1=[%.6f,%.6f,%.6f,%.6f] v2=[%.6f,%.6f,%.6f,%.6f] v3=[%.6f,%.6f,%.6f,%.6f]",
				debugTraceRtHandle,
				static_cast<u32>(_drawFramebuffer),
				static_cast<unsigned long long>(packetCopy.debugPacketId),
				packetSourceName(packetCopy.debugSource),
				packetCopy.debugTexrect ? 1U : 0U,
				transformModeName(packetCopy.transformMode),
				static_cast<unsigned long long>(packetCopy.debugCombinerMux),
				packetCopy.debugCombinerCycleType,
				static_cast<u32>(packetCopy.vertices.size()),
				sampledHandle0,
				sampledHandle0Rt,
				sampledHandle1,
				sampledHandle1Rt,
				minX,
				minY,
				maxX,
				maxY,
				minS0,
				minT0,
				maxS0,
				maxT0,
				minS1,
				minT1,
				maxS1,
				maxT1,
				v0 != nullptr ? v0->x : 0.0f,
				v0 != nullptr ? v0->y : 0.0f,
				v0 != nullptr ? v0->s0 : 0.0f,
				v0 != nullptr ? v0->t0 : 0.0f,
				v1 != nullptr ? v1->x : 0.0f,
				v1 != nullptr ? v1->y : 0.0f,
				v1 != nullptr ? v1->s0 : 0.0f,
				v1 != nullptr ? v1->t0 : 0.0f,
				v2 != nullptr ? v2->x : 0.0f,
				v2 != nullptr ? v2->y : 0.0f,
				v2 != nullptr ? v2->s0 : 0.0f,
				v2 != nullptr ? v2->t0 : 0.0f,
				v3 != nullptr ? v3->x : 0.0f,
				v3 != nullptr ? v3->y : 0.0f,
				v3 != nullptr ? v3->s0 : 0.0f,
				v3 != nullptr ? v3->t0 : 0.0f);
			++debugTraceRtLogCount;
		}
		std::vector<DrawPacket> packets;
		packets.emplace_back(std::move(packetCopy));

		DrawCommandEncoder::EncodeInfo drawInfo{};
		drawInfo.commandBuffer = commandBuffer;
		drawInfo.vertexBuffer = vertexBuffer;
		drawInfo.vertexBufferOffset = 0;
		drawInfo.packets = &packets;
		drawInfo.pipelineCache = &offscreenPipelineCache;
		drawInfo.descriptorBinder = &m_vk->descriptorBinder;
		drawInfo.pipelineLayout = m_vk->programLibrary.getBasicColorPipelineLayout();
		drawInfo.renderExtent = renderExtent;
		drawInfo.wideLinesEnabled = m_vk->wideLinesEnabled;
		drawInfo.maxLineWidth = m_maxLineWidth;
		DrawCommandEncoder::encode(drawInfo);

		vkCmdEndRenderPass(commandBuffer);
		if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
			break;

		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		if (vkQueueSubmit(m_vk->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
			break;
		if (vkQueueWaitIdle(m_vk->graphicsQueue) != VK_SUCCESS)
			break;

		colorTexture->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		colorTexture->hasContent = true;
		colorTexture->renderTargetWritten = true;
		colorTexture->contentWidth = std::max(colorTexture->contentWidth, renderExtent.width);
		colorTexture->contentHeight = std::max(colorTexture->contentHeight, renderExtent.height);
		if (depthTexture != nullptr) {
			depthTexture->imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			depthTexture->hasContent = true;
			depthTexture->renderTargetWritten = true;
			depthTexture->contentWidth = std::max(depthTexture->contentWidth, renderExtent.width);
			depthTexture->contentHeight = std::max(depthTexture->contentHeight, renderExtent.height);
		}
		success = true;
	} while (false);

	if (!success) {
		vkFboTrace(
			"op=draw_offscreen result=0 drawFbo=%u vertices=%u textureMask=0x%x",
			static_cast<u32>(_drawFramebuffer),
			static_cast<u32>(_packet.vertices.size()),
			_packet.textureSlotMask);
	} else if (isVkFboTraceVerboseEnabled()) {
		vkFboTrace(
			"op=draw_offscreen result=1 drawFbo=%u vertices=%u textureMask=0x%x",
			static_cast<u32>(_drawFramebuffer),
			static_cast<u32>(_packet.vertices.size()),
			_packet.textureSlotMask);
	}
	cleanup();
	return success;
#endif
}

f32 ContextImpl::getMaxLineWidth()
{
	return m_maxLineWidth;
}

bool ContextImpl::present()
{
#if !REALITYVK_VULKAN_HEADERS_AVAILABLE
	return false;
#else
	if (!m_coreReady || !m_vk || m_vk->device == VK_NULL_HANDLE || m_vk->swapchain == VK_NULL_HANDLE)
		return false;
	if (m_vk->graphicsQueue == VK_NULL_HANDLE || m_vk->presentQueue == VK_NULL_HANDLE)
		return false;
	if (m_vk->frameSync.empty() || m_vk->frameCommandBuffers.empty())
		return false;
	if (m_vk->frameSyncIndex >= m_vk->frameCommandBuffers.size())
		return false;

	VulkanState::FrameSync & currentFrame = m_vk->frameSync[m_vk->frameSyncIndex];
	VkCommandBuffer commandBuffer = m_vk->frameCommandBuffers[m_vk->frameSyncIndex];
	if (currentFrame.imageAvailable == VK_NULL_HANDLE
		|| currentFrame.renderFinished == VK_NULL_HANDLE
		|| currentFrame.inFlight == VK_NULL_HANDLE
		|| commandBuffer == VK_NULL_HANDLE) {
		return false;
	}

	bool success = false;
	do {
		if (vkWaitForFences(m_vk->device, 1, &currentFrame.inFlight, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkWaitForFences failed.");
			break;
		}

		u32 currentImageIndex = 0;
		const VkResult acquireResult = vkAcquireNextImageKHR(
			m_vk->device,
			m_vk->swapchain,
			UINT64_MAX,
			currentFrame.imageAvailable,
			VK_NULL_HANDLE,
			&currentImageIndex);

		if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
			vkDeviceWaitIdle(m_vk->device);
			destroySwapchain();
			createSwapchain();
			break;
		}

		if (acquireResult != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkAcquireNextImageKHR failed: %d", static_cast<int>(acquireResult));
			break;
		}

		if (currentImageIndex >= m_vk->swapchainImageFences.size()) {
			LOG(LOG_WARNING, "Acquired Vulkan swapchain image index is out of range: %u", currentImageIndex);
			break;
		}
		if (currentImageIndex >= m_vk->swapchainFramebuffers.size()) {
			LOG(LOG_WARNING, "Acquired Vulkan framebuffer index is out of range: %u", currentImageIndex);
			break;
		}
		if (m_vk->renderPass == VK_NULL_HANDLE) {
			LOG(LOG_WARNING, "Vulkan render pass is not initialized.");
			break;
		}

		VkFence & imageFence = m_vk->swapchainImageFences[currentImageIndex];
		if (imageFence != VK_NULL_HANDLE && imageFence != currentFrame.inFlight) {
			if (vkWaitForFences(m_vk->device, 1, &imageFence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
				LOG(LOG_WARNING, "vkWaitForFences failed for swapchain image.");
				break;
			}
		}

		if (vkResetCommandBuffer(commandBuffer, 0) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkResetCommandBuffer failed.");
			break;
		}

			const std::vector<DrawPacket> & queuedPackets = m_vk->drawRecorder.packets();
			std::vector<DrawPacket> presentPackets;
			presentPackets.reserve(queuedPackets.size() + 1U);
			size_t totalVertexCount = 0;
			static const u32 debugTraceRtHandle = []() -> u32 {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_TRACE_RT_HANDLE");
				if (env == nullptr || env[0] == '\0')
					return 0U;
				return static_cast<u32>(std::strtoul(env, nullptr, 10));
			}();
			static u32 debugTraceRtReadLogCount = 0U;
			static const std::vector<u32> debugTraceFinalLayerHandles = []() -> std::vector<u32> {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_TRACE_FINAL_LAYER_HANDLES");
				std::vector<u32> handles;
				if (env == nullptr || env[0] == '\0')
					return handles;
				const char * cursor = env;
				while (*cursor != '\0') {
					while (*cursor == ',' || *cursor == ' ' || *cursor == '\t')
						++cursor;
					if (*cursor == '\0')
						break;
					char * end = nullptr;
					const unsigned long value = std::strtoul(cursor, &end, 0);
					if (end == cursor) {
						while (*cursor != '\0' && *cursor != ',')
							++cursor;
						continue;
					}
					handles.push_back(static_cast<u32>(value));
					cursor = end;
				}
				std::sort(handles.begin(), handles.end());
				handles.erase(std::unique(handles.begin(), handles.end()), handles.end());
				return handles;
			}();
			static const bool debugTraceFinalLayers = !debugTraceFinalLayerHandles.empty();
			static const u32 debugTraceFinalLayerLimit = []() -> u32 {
				const char * env = std::getenv("REALITYVK_VK_DEBUG_TRACE_FINAL_LAYER_LIMIT");
				if (env == nullptr || env[0] == '\0')
					return 96U;
				return static_cast<u32>(std::strtoul(env, nullptr, 10));
			}();
			static u32 debugTraceFinalLayerLogCount = 0U;

			auto packetMatchesFinalLayerHandle = [&](const DrawPacket & _packet, u32 & _matchedHandle, u32 & _matchedSlot, bool & _matchedHandleIsRt) -> bool {
				_matchedHandle = 0U;
				_matchedSlot = 0U;
				_matchedHandleIsRt = false;
				if (!debugTraceFinalLayers)
					return false;
				for (u32 slot = 0U; slot < 2U; ++slot) {
					if ((_packet.textureSlotMask & (1U << slot)) == 0U)
						continue;
					const u32 handle = static_cast<u32>(_packet.textureSlots[slot].texture);
					if (!std::binary_search(debugTraceFinalLayerHandles.begin(), debugTraceFinalLayerHandles.end(), handle))
						continue;
					_matchedHandle = handle;
					_matchedSlot = slot;
					const TextureStore::TextureResource * texture = m_vk->textureStore.getTexture(_packet.textureSlots[slot].texture);
					_matchedHandleIsRt = texture != nullptr && texture->renderTargetWritten;
					return true;
				}
				return false;
			};

			auto packetIsFullscreen = [](const DrawPacket & _packet) -> bool {
				if (_packet.vertices.empty())
					return false;
				f32 minX = _packet.vertices[0].x;
				f32 minY = _packet.vertices[0].y;
				f32 maxX = minX;
				f32 maxY = minY;
				for (const DrawVertex & vertex : _packet.vertices) {
					minX = std::min(minX, vertex.x);
					minY = std::min(minY, vertex.y);
					maxX = std::max(maxX, vertex.x);
					maxY = std::max(maxY, vertex.y);
				}
				const f32 width = maxX - minX;
				const f32 height = maxY - minY;
				return width >= 1.70f && height >= 1.70f;
			};

				for (const DrawPacket & packet : queuedPackets) {
					DrawPacket normalizePacket = packet;
					normalizePacket.state.raster.viewportX = 0;
				normalizePacket.state.raster.viewportY = 0;
				normalizePacket.state.raster.viewportWidth = static_cast<s32>(m_vk->swapchainExtent.width);
				normalizePacket.state.raster.viewportHeight = static_cast<s32>(m_vk->swapchainExtent.height);
				normalizePacket.state.raster.viewportValid = true;
				normalizePacket.forceRasterRectTransform = normalizePacket.transformMode == vulkan::VertexTransformMode::Rect;
					DrawPacket packetCopy = packet;
						packetCopy.vertices.clear();
						packetCopy.vertices.reserve(packet.vertices.size());
						for (const DrawVertex & vertex : packet.vertices)
							packetCopy.vertices.push_back(vulkan::packet_normalize::normalizePacketVertex(normalizePacket, vertex));
					bool appliedRtTexrectFlip = false;
					u32 appliedRtTexrectFlipSlotMask = 0U;

					static const bool autoFlipRtTexrectY = std::getenv("REALITYVK_VK_DISABLE_RT_TEXRECT_Y_FLIP") == nullptr;
					static const bool debugFlipRtTexrectY = std::getenv("REALITYVK_VK_DEBUG_FLIP_RT_TEXRECT_Y") != nullptr;
					static const u32 debugFlipRtTexrectHandle = []() -> u32 {
						const char * env = std::getenv("REALITYVK_VK_DEBUG_FLIP_RT_TEXRECT_HANDLE");
						if (env == nullptr || env[0] == '\0')
							return 0U;
						return static_cast<u32>(std::strtoul(env, nullptr, 10));
					}();
					if ((autoFlipRtTexrectY || debugFlipRtTexrectY)
						&& packetCopy.debugTexrect
						&& !packetCopy.vertices.empty()) {
						bool sampleRtTexture = false;
						u32 sampledRtSlotMask = 0U;
						const bool filterByDebugHandle = debugFlipRtTexrectY && debugFlipRtTexrectHandle != 0U;
						bool hasTargetHandle = !filterByDebugHandle;
						for (u32 slot = 0U; slot < 2U; ++slot) {
							if ((packetCopy.textureSlotMask & (1U << slot)) == 0U)
								continue;
							const graphics::ObjectHandle handle = packetCopy.textureSlots[slot].texture;
							if (filterByDebugHandle && static_cast<u32>(handle) == debugFlipRtTexrectHandle)
								hasTargetHandle = true;
							const TextureStore::TextureResource * texture = m_vk->textureStore.getTexture(handle);
							if (texture != nullptr && texture->renderTargetWritten) {
								sampleRtTexture = true;
								if (!filterByDebugHandle || static_cast<u32>(handle) == debugFlipRtTexrectHandle)
									sampledRtSlotMask |= (1U << slot);
								if (hasTargetHandle)
									break;
							}
						}
						if (sampleRtTexture && hasTargetHandle && sampledRtSlotMask != 0U) {
							for (DrawVertex & vertex : packetCopy.vertices) {
								if ((sampledRtSlotMask & (1U << 0U)) != 0U)
									vertex.t0 = 1.0f - vertex.t0;
								if ((sampledRtSlotMask & (1U << 1U)) != 0U)
									vertex.t1 = 1.0f - vertex.t1;
							}
							appliedRtTexrectFlip = true;
							appliedRtTexrectFlipSlotMask = sampledRtSlotMask;
						}
					}

					static const bool debugFlipPresentTexrectY = std::getenv("REALITYVK_VK_DEBUG_FLIP_PRESENT_TEXRECT_Y") != nullptr;
					if (debugFlipPresentTexrectY
						&& packetCopy.debugSource == kPacketSourceRects
						&& packetCopy.debugTexrect
						&& (packetCopy.textureSlotMask & (1U << 0)) != 0U
						&& !packetCopy.vertices.empty()) {
						f32 minT0 = packetCopy.vertices[0].t0;
						f32 maxT0 = minT0;
						for (const DrawVertex & vertex : packetCopy.vertices) {
							minT0 = std::min(minT0, vertex.t0);
							maxT0 = std::max(maxT0, vertex.t0);
						}
						// Only flip when texcoords are normalized-like.
						if (minT0 >= -0.25f && maxT0 <= 1.25f) {
							for (DrawVertex & vertex : packetCopy.vertices) {
								vertex.t0 = 1.0f - vertex.t0;
								vertex.t1 = 1.0f - vertex.t1;
							}
						}
					}
					static const bool canonicalizeFinalRtTexrectPos = std::getenv("REALITYVK_VK_DISABLE_CANONICAL_FINAL_RT_TEXRECT_POS") == nullptr;
					static const bool canonicalizeFinalRtTexrectUv = std::getenv("REALITYVK_VK_DISABLE_CANONICAL_FINAL_RT_TEXRECT_UV") == nullptr;
					if (canonicalizeFinalRtTexrectPos
						&& packetCopy.debugTexrect
						&& packetIsFullscreen(packetCopy)
						&& packetCopy.vertices.size() == 4U) {
						bool sampledRtTexture = false;
						for (u32 slot = 0U; slot < 2U; ++slot) {
							if ((packetCopy.textureSlotMask & (1U << slot)) == 0U)
								continue;
							const TextureStore::TextureResource * texture = m_vk->textureStore.getTexture(packetCopy.textureSlots[slot].texture);
							if (texture != nullptr && texture->renderTargetWritten) {
								sampledRtTexture = true;
								break;
							}
						}
						if (sampledRtTexture) {
							packetCopy.vertices[0].x = -1.0f;
							packetCopy.vertices[0].y = 1.0f;
							packetCopy.vertices[1].x = 1.0f;
							packetCopy.vertices[1].y = 1.0f;
							packetCopy.vertices[2].x = -1.0f;
							packetCopy.vertices[2].y = -1.0f;
							packetCopy.vertices[3].x = 1.0f;
							packetCopy.vertices[3].y = -1.0f;
						}
					}
					if (canonicalizeFinalRtTexrectUv
						&& packetCopy.debugTexrect
						&& packetIsFullscreen(packetCopy)
						&& packetCopy.vertices.size() == 4U) {
						bool sampledRtTexture0 = false;
						bool sampledRtTexture1 = false;
						if ((packetCopy.textureSlotMask & (1U << 0U)) != 0U) {
							const TextureStore::TextureResource * texture0 = m_vk->textureStore.getTexture(packetCopy.textureSlots[0].texture);
							sampledRtTexture0 = texture0 != nullptr && texture0->renderTargetWritten;
						}
						if ((packetCopy.textureSlotMask & (1U << 1U)) != 0U) {
							const TextureStore::TextureResource * texture1 = m_vk->textureStore.getTexture(packetCopy.textureSlots[1].texture);
							sampledRtTexture1 = texture1 != nullptr && texture1->renderTargetWritten;
						}
						if (sampledRtTexture0) {
							packetCopy.vertices[0].s0 = 0.0f;
							packetCopy.vertices[1].s0 = 1.0f;
							packetCopy.vertices[2].s0 = 0.0f;
							packetCopy.vertices[3].s0 = 1.0f;
							const bool flipT0 = (appliedRtTexrectFlipSlotMask & (1U << 0U)) != 0U;
							packetCopy.vertices[0].t0 = flipT0 ? 1.0f : 0.0f;
							packetCopy.vertices[1].t0 = flipT0 ? 1.0f : 0.0f;
							packetCopy.vertices[2].t0 = flipT0 ? 0.0f : 1.0f;
							packetCopy.vertices[3].t0 = flipT0 ? 0.0f : 1.0f;
						}
						if (sampledRtTexture1) {
							packetCopy.vertices[0].s1 = 0.0f;
							packetCopy.vertices[1].s1 = 1.0f;
							packetCopy.vertices[2].s1 = 0.0f;
							packetCopy.vertices[3].s1 = 1.0f;
							const bool flipT1 = (appliedRtTexrectFlipSlotMask & (1U << 1U)) != 0U;
							packetCopy.vertices[0].t1 = flipT1 ? 1.0f : 0.0f;
							packetCopy.vertices[1].t1 = flipT1 ? 1.0f : 0.0f;
							packetCopy.vertices[2].t1 = flipT1 ? 0.0f : 1.0f;
							packetCopy.vertices[3].t1 = flipT1 ? 0.0f : 1.0f;
						}
					}

					// Packets can carry upscaled viewports (for example 2880x2880 on a 1440x1080 swapchain).
					// The present path already uploads clip-space vertices, so use swapchain viewport extents.
				packetCopy.state.raster.viewportX = 0;
				packetCopy.state.raster.viewportY = 0;
				packetCopy.state.raster.viewportWidth = static_cast<s32>(m_vk->swapchainExtent.width);
				packetCopy.state.raster.viewportHeight = static_cast<s32>(m_vk->swapchainExtent.height);
				packetCopy.state.raster.viewportValid = true;
				packetCopy.dirtyMask |= draw_dirty::kViewport;
				static const bool debugDisablePresentScissor = std::getenv("REALITYVK_VK_DEBUG_DISABLE_PRESENT_SCISSOR") != nullptr;
				if (debugDisablePresentScissor) {
					packetCopy.state.raster.scissorEnabled = false;
					packetCopy.dirtyMask |= draw_dirty::kScissor;
				}

				if (debugTraceRtHandle != 0U && debugTraceRtReadLogCount < 256U) {
					for (u32 slot = 0U; slot < 2U; ++slot) {
						if ((packetCopy.textureSlotMask & (1U << slot)) == 0U)
							continue;
						if (static_cast<u32>(packetCopy.textureSlots[slot].texture) != debugTraceRtHandle)
							continue;
						f32 minX = 0.0f;
						f32 minY = 0.0f;
						f32 maxX = 0.0f;
						f32 maxY = 0.0f;
						f32 minS0 = 0.0f;
						f32 maxS0 = 0.0f;
						f32 minT0 = 0.0f;
						f32 maxT0 = 0.0f;
						f32 minS1 = 0.0f;
						f32 maxS1 = 0.0f;
						f32 minT1 = 0.0f;
						f32 maxT1 = 0.0f;
						if (!packetCopy.vertices.empty()) {
							minX = maxX = packetCopy.vertices[0].x;
							minY = maxY = packetCopy.vertices[0].y;
							minS0 = maxS0 = packetCopy.vertices[0].s0;
							minT0 = maxT0 = packetCopy.vertices[0].t0;
							minS1 = maxS1 = packetCopy.vertices[0].s1;
							minT1 = maxT1 = packetCopy.vertices[0].t1;
							for (const DrawVertex & vertex : packetCopy.vertices) {
								minX = std::min(minX, vertex.x);
								maxX = std::max(maxX, vertex.x);
								minY = std::min(minY, vertex.y);
								maxY = std::max(maxY, vertex.y);
								minS0 = std::min(minS0, vertex.s0);
								maxS0 = std::max(maxS0, vertex.s0);
								minT0 = std::min(minT0, vertex.t0);
								maxT0 = std::max(maxT0, vertex.t0);
								minS1 = std::min(minS1, vertex.s1);
								maxS1 = std::max(maxS1, vertex.s1);
								minT1 = std::min(minT1, vertex.t1);
								maxT1 = std::max(maxT1, vertex.t1);
							}
						}
						const DrawVertex * v0 = packetCopy.vertices.size() > 0 ? &packetCopy.vertices[0] : nullptr;
						const DrawVertex * v1 = packetCopy.vertices.size() > 1 ? &packetCopy.vertices[1] : nullptr;
						const DrawVertex * v2 = packetCopy.vertices.size() > 2 ? &packetCopy.vertices[2] : nullptr;
						const DrawVertex * v3 = packetCopy.vertices.size() > 3 ? &packetCopy.vertices[3] : nullptr;
						LOG(
							LOG_WARNING,
							"VK RT trace(read): handle=%u slot=%u packetId=%llu src=%s texrect=%u mode=%s mux=0x%016llx cycle=%u vertices=%u pos=[%.3f,%.3f]-[%.3f,%.3f] tex0=[%.3f,%.3f]-[%.3f,%.3f] tex1=[%.3f,%.3f]-[%.3f,%.3f] v0=[%.3f,%.3f,%.3f,%.3f] v1=[%.3f,%.3f,%.3f,%.3f] v2=[%.3f,%.3f,%.3f,%.3f] v3=[%.3f,%.3f,%.3f,%.3f]",
							debugTraceRtHandle,
							slot,
							static_cast<unsigned long long>(packetCopy.debugPacketId),
							packetSourceName(packetCopy.debugSource),
							packetCopy.debugTexrect ? 1U : 0U,
							transformModeName(packetCopy.transformMode),
							static_cast<unsigned long long>(packetCopy.debugCombinerMux),
							packetCopy.debugCombinerCycleType,
							static_cast<u32>(packetCopy.vertices.size()),
							minX,
							minY,
							maxX,
							maxY,
							minS0,
							minT0,
							maxS0,
							maxT0,
							minS1,
							minT1,
							maxS1,
							maxT1,
							v0 != nullptr ? v0->x : 0.0f,
							v0 != nullptr ? v0->y : 0.0f,
							v0 != nullptr ? v0->s0 : 0.0f,
							v0 != nullptr ? v0->t0 : 0.0f,
							v1 != nullptr ? v1->x : 0.0f,
							v1 != nullptr ? v1->y : 0.0f,
							v1 != nullptr ? v1->s0 : 0.0f,
							v1 != nullptr ? v1->t0 : 0.0f,
							v2 != nullptr ? v2->x : 0.0f,
							v2 != nullptr ? v2->y : 0.0f,
							v2 != nullptr ? v2->s0 : 0.0f,
							v2 != nullptr ? v2->t0 : 0.0f,
							v3 != nullptr ? v3->x : 0.0f,
							v3 != nullptr ? v3->y : 0.0f,
							v3 != nullptr ? v3->s0 : 0.0f,
							v3 != nullptr ? v3->t0 : 0.0f);
						++debugTraceRtReadLogCount;
						break;
					}
				}
				if (debugTraceFinalLayers && debugTraceFinalLayerLogCount < debugTraceFinalLayerLimit) {
					u32 matchedHandle = 0U;
					u32 matchedSlot = 0U;
					bool matchedHandleIsRt = false;
					if (packetMatchesFinalLayerHandle(packetCopy, matchedHandle, matchedSlot, matchedHandleIsRt)) {
						f32 minX = 0.0f;
						f32 minY = 0.0f;
						f32 maxX = 0.0f;
						f32 maxY = 0.0f;
						f32 minS0 = 0.0f;
						f32 maxS0 = 0.0f;
						f32 minT0 = 0.0f;
						f32 maxT0 = 0.0f;
						f32 minS1 = 0.0f;
						f32 maxS1 = 0.0f;
						f32 minT1 = 0.0f;
						f32 maxT1 = 0.0f;
						if (!packetCopy.vertices.empty()) {
							minX = maxX = packetCopy.vertices[0].x;
							minY = maxY = packetCopy.vertices[0].y;
							minS0 = maxS0 = packetCopy.vertices[0].s0;
							minT0 = maxT0 = packetCopy.vertices[0].t0;
							minS1 = maxS1 = packetCopy.vertices[0].s1;
							minT1 = maxT1 = packetCopy.vertices[0].t1;
							for (const DrawVertex & vertex : packetCopy.vertices) {
								minX = std::min(minX, vertex.x);
								maxX = std::max(maxX, vertex.x);
								minY = std::min(minY, vertex.y);
								maxY = std::max(maxY, vertex.y);
								minS0 = std::min(minS0, vertex.s0);
								maxS0 = std::max(maxS0, vertex.s0);
								minT0 = std::min(minT0, vertex.t0);
								maxT0 = std::max(maxT0, vertex.t0);
								minS1 = std::min(minS1, vertex.s1);
								maxS1 = std::max(maxS1, vertex.s1);
								minT1 = std::min(minT1, vertex.t1);
								maxT1 = std::max(maxT1, vertex.t1);
							}
						}
						const bool fullscreenPacket = packetIsFullscreen(packetCopy);
						const char * layerLabel = packetCopy.debugTexrect
							? (fullscreenPacket ? "final_fullscreen_texrect" : "final_partial_texrect")
							: "final_non_texrect";
						LOG(
							LOG_WARNING,
							"VK composite trace(final): label=%s handle=%u slot=%u packetId=%llu src=%s texrect=%u mode=%s fullscreen=%u rtSample=%u flipApplied=%u flipSlots=0x%x strictBlend=%u blendEnabled=%u shaderFlags=0x%08x mux=0x%016llx cycle=%u vertices=%u pos=[%.3f,%.3f]-[%.3f,%.3f] tex0=[%.3f,%.3f]-[%.3f,%.3f] tex1=[%.3f,%.3f]-[%.3f,%.3f] viewport=%d,%d,%d,%d valid=%u scissor=%d,%d,%d,%d enabled=%u",
							layerLabel,
							matchedHandle,
							matchedSlot,
							static_cast<unsigned long long>(packetCopy.debugPacketId),
							packetSourceName(packetCopy.debugSource),
							packetCopy.debugTexrect ? 1U : 0U,
							transformModeName(packetCopy.transformMode),
							fullscreenPacket ? 1U : 0U,
							matchedHandleIsRt ? 1U : 0U,
							appliedRtTexrectFlip ? 1U : 0U,
							appliedRtTexrectFlipSlotMask,
							(packetCopy.shaderFlags & vulkan::draw_shader_flags::kSpecialStrictBlendMux) != 0U ? 1U : 0U,
							packetCopy.state.blend.enabled ? 1U : 0U,
							packetCopy.shaderFlags,
							static_cast<unsigned long long>(packetCopy.debugCombinerMux),
							packetCopy.debugCombinerCycleType,
							static_cast<u32>(packetCopy.vertices.size()),
							minX,
							minY,
							maxX,
							maxY,
							minS0,
							minT0,
							maxS0,
							maxT0,
							minS1,
							minT1,
							maxS1,
							maxT1,
							packetCopy.state.raster.viewportX,
							packetCopy.state.raster.viewportY,
							packetCopy.state.raster.viewportWidth,
							packetCopy.state.raster.viewportHeight,
							packetCopy.state.raster.viewportValid ? 1U : 0U,
							packetCopy.state.raster.scissorX,
							packetCopy.state.raster.scissorY,
							packetCopy.state.raster.scissorWidth,
							packetCopy.state.raster.scissorHeight,
							packetCopy.state.raster.scissorEnabled ? 1U : 0U);
						++debugTraceFinalLayerLogCount;
					}
				}

				totalVertexCount += packetCopy.vertices.size();
				presentPackets.emplace_back(std::move(packetCopy));
			}

			const bool hasDrawPackets = totalVertexCount > 0;
			if (isVkFboTraceEnabled()) {
				vkFboTrace(
					"op=present packets=%u vertices=%u skippedOffscreenDraws=%llu skippedOffscreenVertices=%llu skippedSuspicious=%u",
					static_cast<u32>(presentPackets.size()),
					static_cast<u32>(totalVertexCount),
					static_cast<unsigned long long>(offscreenSkippedDrawCalls()),
					static_cast<unsigned long long>(offscreenSkippedVertices()),
					0U);
			}
				static const bool debugPresent = std::getenv("REALITYVK_VK_DEBUG_PRESENT") != nullptr;
				static const bool debugPresentPackets = std::getenv("REALITYVK_VK_DEBUG_PRESENT_PACKETS") != nullptr;
				if (debugPresent) {
				f32 minX = 0.0f;
				f32 minY = 0.0f;
				f32 maxX = 0.0f;
				f32 maxY = 0.0f;
				f32 minR = 0.0f;
				f32 minG = 0.0f;
				f32 minB = 0.0f;
				f32 minA = 0.0f;
				f32 maxR = 0.0f;
				f32 maxG = 0.0f;
				f32 maxB = 0.0f;
				f32 maxA = 0.0f;
				f32 minS0 = 0.0f;
				f32 minT0 = 0.0f;
				f32 maxS0 = 0.0f;
				f32 maxT0 = 0.0f;
				f32 minS1 = 0.0f;
				f32 minT1 = 0.0f;
				f32 maxS1 = 0.0f;
				f32 maxT1 = 0.0f;
				f32 minW = 0.0f;
				f32 maxW = 0.0f;
				f32 sampleW = 0.0f;
				f32 sampleZ = 0.0f;
				f32 minXOverW = 0.0f;
				f32 maxXOverW = 0.0f;
				f32 minYOverW = 0.0f;
				f32 maxYOverW = 0.0f;
				bool hasAnyVertex = false;

					for (const DrawPacket & packet : presentPackets) {
						for (const DrawVertex & vertex : packet.vertices) {
						const f32 invW = std::abs(vertex.w) > 1.0e-6f ? (1.0f / vertex.w) : 0.0f;
						const f32 xOverW = vertex.x * invW;
						const f32 yOverW = vertex.y * invW;
						if (!hasAnyVertex) {
							minX = maxX = vertex.x;
							minY = maxY = vertex.y;
							minR = maxR = vertex.r;
							minG = maxG = vertex.g;
							minB = maxB = vertex.b;
							minA = maxA = vertex.a;
							minS0 = maxS0 = vertex.s0;
							minT0 = maxT0 = vertex.t0;
							minS1 = maxS1 = vertex.s1;
							minT1 = maxT1 = vertex.t1;
							minW = maxW = vertex.w;
							sampleW = vertex.w;
							sampleZ = vertex.z;
							minXOverW = maxXOverW = xOverW;
							minYOverW = maxYOverW = yOverW;
							hasAnyVertex = true;
							continue;
						}
						minX = std::min(minX, vertex.x);
						minY = std::min(minY, vertex.y);
						maxX = std::max(maxX, vertex.x);
						maxY = std::max(maxY, vertex.y);
						minR = std::min(minR, vertex.r);
						minG = std::min(minG, vertex.g);
						minB = std::min(minB, vertex.b);
						minA = std::min(minA, vertex.a);
						maxR = std::max(maxR, vertex.r);
						maxG = std::max(maxG, vertex.g);
						maxB = std::max(maxB, vertex.b);
						maxA = std::max(maxA, vertex.a);
						minS0 = std::min(minS0, vertex.s0);
						minT0 = std::min(minT0, vertex.t0);
						maxS0 = std::max(maxS0, vertex.s0);
						maxT0 = std::max(maxT0, vertex.t0);
						minS1 = std::min(minS1, vertex.s1);
						minT1 = std::min(minT1, vertex.t1);
						maxS1 = std::max(maxS1, vertex.s1);
						maxT1 = std::max(maxT1, vertex.t1);
						minW = std::min(minW, vertex.w);
						maxW = std::max(maxW, vertex.w);
						minXOverW = std::min(minXOverW, xOverW);
						maxXOverW = std::max(maxXOverW, xOverW);
						minYOverW = std::min(minYOverW, yOverW);
						maxYOverW = std::max(maxYOverW, yOverW);
						}
					}
					if (debugPresentPackets) {
						static const u32 presentPacketLogLimit = []() -> u32 {
							const char * env = std::getenv("REALITYVK_VK_DEBUG_PRESENT_PACKET_LIMIT");
							if (env == nullptr || env[0] == '\0')
								return 128U;
							return static_cast<u32>(std::strtoul(env, nullptr, 10));
						}();
						static u32 presentPacketLogCount = 0U;
						for (const DrawPacket & packet : presentPackets) {
							if (presentPacketLogCount >= presentPacketLogLimit)
								break;
							if (packet.vertices.empty())
								continue;
							f32 pMinX = packet.vertices[0].x;
							f32 pMinY = packet.vertices[0].y;
							f32 pMaxX = pMinX;
							f32 pMaxY = pMinY;
							f32 pMinS0 = packet.vertices[0].s0;
							f32 pMinT0 = packet.vertices[0].t0;
							f32 pMaxS0 = pMinS0;
							f32 pMaxT0 = pMinT0;
							f32 pMinR = packet.vertices[0].r;
							f32 pMinG = packet.vertices[0].g;
							f32 pMinB = packet.vertices[0].b;
							f32 pMinA = packet.vertices[0].a;
							f32 pMaxR = pMinR;
							f32 pMaxG = pMinG;
							f32 pMaxB = pMinB;
							f32 pMaxA = pMinA;
							for (const DrawVertex & vertex : packet.vertices) {
								pMinX = std::min(pMinX, vertex.x);
								pMinY = std::min(pMinY, vertex.y);
								pMaxX = std::max(pMaxX, vertex.x);
								pMaxY = std::max(pMaxY, vertex.y);
								pMinS0 = std::min(pMinS0, vertex.s0);
								pMinT0 = std::min(pMinT0, vertex.t0);
								pMaxS0 = std::max(pMaxS0, vertex.s0);
								pMaxT0 = std::max(pMaxT0, vertex.t0);
								pMinR = std::min(pMinR, vertex.r);
								pMinG = std::min(pMinG, vertex.g);
								pMinB = std::min(pMinB, vertex.b);
								pMinA = std::min(pMinA, vertex.a);
								pMaxR = std::max(pMaxR, vertex.r);
								pMaxG = std::max(pMaxG, vertex.g);
								pMaxB = std::max(pMaxB, vertex.b);
								pMaxA = std::max(pMaxA, vertex.a);
							}
							LOG(
								LOG_WARNING,
								"VK present packet debug: id=%llu src=%s texrect=%u mux=0x%016llx cycle=%u vertices=%u shaderFlags=0x%08x texMask=0x%08x handle0=%u handle1=%u pos=[%.3f,%.3f]-[%.3f,%.3f] color=[%.3f,%.3f,%.3f,%.3f]-[%.3f,%.3f,%.3f,%.3f] tex0=[%.3f,%.3f]-[%.3f,%.3f]",
								static_cast<unsigned long long>(packet.debugPacketId),
								packetSourceName(packet.debugSource),
								packet.debugTexrect ? 1U : 0U,
								static_cast<unsigned long long>(packet.debugCombinerMux),
								packet.debugCombinerCycleType,
								static_cast<u32>(packet.vertices.size()),
								packet.shaderFlags,
								packet.textureSlotMask,
								(packet.textureSlotMask & (1U << 0)) != 0U ? static_cast<u32>(packet.textureSlots[0].texture) : 0U,
								(packet.textureSlotMask & (1U << 1)) != 0U ? static_cast<u32>(packet.textureSlots[1].texture) : 0U,
								pMinX,
								pMinY,
								pMaxX,
								pMaxY,
								pMinR,
								pMinG,
								pMinB,
								pMinA,
								pMaxR,
								pMaxG,
								pMaxB,
								pMaxA,
								pMinS0,
								pMinT0,
								pMaxS0,
								pMaxT0);
							++presentPacketLogCount;
						}
					}

					LOG(
						LOG_WARNING,
					"VK present debug: packets=%u vertices=%zu hasDraw=%u bounds=[%.2f,%.2f]-[%.2f,%.2f] tex0=[%.3f,%.3f]-[%.3f,%.3f] tex1=[%.3f,%.3f]-[%.3f,%.3f] color=[%.3f,%.3f,%.3f,%.3f]-[%.3f,%.3f,%.3f,%.3f] w=[%.4f,%.4f] ndc=[%.3f,%.3f]-[%.3f,%.3f] sampleW=%.4f sampleZ=%.4f swap=%ux%u",
					static_cast<unsigned>(presentPackets.size()),
					totalVertexCount,
					hasDrawPackets ? 1U : 0U,
					hasAnyVertex ? minX : 0.0f,
					hasAnyVertex ? minY : 0.0f,
					hasAnyVertex ? maxX : 0.0f,
					hasAnyVertex ? maxY : 0.0f,
					hasAnyVertex ? minS0 : 0.0f,
					hasAnyVertex ? minT0 : 0.0f,
					hasAnyVertex ? maxS0 : 0.0f,
					hasAnyVertex ? maxT0 : 0.0f,
					hasAnyVertex ? minS1 : 0.0f,
					hasAnyVertex ? minT1 : 0.0f,
					hasAnyVertex ? maxS1 : 0.0f,
					hasAnyVertex ? maxT1 : 0.0f,
					hasAnyVertex ? minR : 0.0f,
					hasAnyVertex ? minG : 0.0f,
					hasAnyVertex ? minB : 0.0f,
					hasAnyVertex ? minA : 0.0f,
					hasAnyVertex ? maxR : 0.0f,
					hasAnyVertex ? maxG : 0.0f,
					hasAnyVertex ? maxB : 0.0f,
					hasAnyVertex ? maxA : 0.0f,
					hasAnyVertex ? minW : 0.0f,
					hasAnyVertex ? maxW : 0.0f,
					hasAnyVertex ? minXOverW : 0.0f,
					hasAnyVertex ? minYOverW : 0.0f,
					hasAnyVertex ? maxXOverW : 0.0f,
					hasAnyVertex ? maxYOverW : 0.0f,
					hasAnyVertex ? sampleW : 0.0f,
					hasAnyVertex ? sampleZ : 0.0f,
					m_vk->swapchainExtent.width,
					m_vk->swapchainExtent.height);
			}

			UploadArena::Allocation uploadAllocation{};
			if (hasDrawPackets) {
				const VkDeviceSize vertexBytes = static_cast<VkDeviceSize>(totalVertexCount * sizeof(DrawVertex));
			m_vk->uploadArena.beginFrame(m_vk->frameSyncIndex);
			if (!m_vk->uploadArena.allocate(
				m_vk->frameSyncIndex,
				vertexBytes,
				static_cast<VkDeviceSize>(alignof(DrawVertex)),
				uploadAllocation)) {
				LOG(LOG_WARNING, "Failed to allocate Vulkan upload arena range.");
				break;
			}
			if (uploadAllocation.buffer == VK_NULL_HANDLE || uploadAllocation.mapped == nullptr) {
				LOG(LOG_WARNING, "Vulkan upload arena returned an invalid allocation.");
				break;
			}

				DrawVertex * dst = static_cast<DrawVertex *>(uploadAllocation.mapped);
				for (const DrawPacket & packet : presentPackets) {
					if (packet.vertices.empty())
						continue;
					for (const DrawVertex & vertex : packet.vertices) {
						*dst = vertex;
						++dst;
					}
				}
		}

		VkCommandBufferBeginInfo commandBufferBeginInfo{};
		commandBufferBeginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		commandBufferBeginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkBeginCommandBuffer failed.");
			break;
		}

		const std::array<f32, 4> & clearColor = m_vk->drawRecorder.clearColor();
		VkClearValue clearValues[2]{};
		clearValues[0].color.float32[0] = clearColor[0];
		clearValues[0].color.float32[1] = clearColor[1];
		clearValues[0].color.float32[2] = clearColor[2];
		clearValues[0].color.float32[3] = clearColor[3];
		clearValues[1].depthStencil.depth = m_vk->drawRecorder.clearDepth();
		clearValues[1].depthStencil.stencil = 0;
		VkRenderPassBeginInfo renderPassBeginInfo{};
		renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		renderPassBeginInfo.renderPass = m_vk->renderPass;
		renderPassBeginInfo.framebuffer = m_vk->swapchainFramebuffers[currentImageIndex];
		renderPassBeginInfo.renderArea.offset = { 0, 0 };
		renderPassBeginInfo.renderArea.extent = m_vk->swapchainExtent;
		renderPassBeginInfo.clearValueCount = 2;
		renderPassBeginInfo.pClearValues = clearValues;
		vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

			if (hasDrawPackets) {
				m_vk->descriptorBinder.beginFrame(m_vk->frameSyncIndex);
				DrawCommandEncoder::EncodeInfo drawInfo{};
			drawInfo.commandBuffer = commandBuffer;
			drawInfo.vertexBuffer = uploadAllocation.buffer;
			drawInfo.vertexBufferOffset = uploadAllocation.offset;
				drawInfo.packets = &presentPackets;
			drawInfo.pipelineCache = &m_vk->pipelineCache;
			drawInfo.descriptorBinder = &m_vk->descriptorBinder;
			drawInfo.pipelineLayout = m_vk->programLibrary.getBasicColorPipelineLayout();
			drawInfo.renderExtent = m_vk->swapchainExtent;
			drawInfo.wideLinesEnabled = m_vk->wideLinesEnabled;
			drawInfo.maxLineWidth = m_maxLineWidth;
			DrawCommandEncoder::encode(drawInfo);
		}

		vkCmdEndRenderPass(commandBuffer);
		if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkEndCommandBuffer failed.");
			break;
		}

		if (vkResetFences(m_vk->device, 1, &currentFrame.inFlight) != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkResetFences failed.");
			break;
		}
		imageFence = currentFrame.inFlight;

		VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo submitInfo{};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = &currentFrame.imageAvailable;
		submitInfo.pWaitDstStageMask = &waitStage;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = &currentFrame.renderFinished;

		const VkResult submitResult = vkQueueSubmit(m_vk->graphicsQueue, 1, &submitInfo, currentFrame.inFlight);
		if (submitResult != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkQueueSubmit failed: %d", static_cast<int>(submitResult));
			break;
		}

		VkPresentInfoKHR presentInfo{};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = &currentFrame.renderFinished;
		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &m_vk->swapchain;
		presentInfo.pImageIndices = &currentImageIndex;

		const VkResult presentResult = vkQueuePresentKHR(m_vk->presentQueue, &presentInfo);
		if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
			vkDeviceWaitIdle(m_vk->device);
			destroySwapchain();
			createSwapchain();
			break;
		}
		if (presentResult != VK_SUCCESS) {
			LOG(LOG_WARNING, "vkQueuePresentKHR failed: %d", static_cast<int>(presentResult));
			break;
		}

		m_vk->lastPresentedImageIndex = currentImageIndex;
		m_vk->frameSyncIndex = (m_vk->frameSyncIndex + 1U) % static_cast<u32>(m_vk->frameSync.size());
		success = true;
	} while (false);

	resetFrameRenderData();
	return success;
#endif
}

bool ContextImpl::isSupported(graphics::SpecialFeatures _feature) const
{
	static const bool experimentalFetchBlendAll = std::getenv("REALITYVK_VK_EXPERIMENTAL_FETCH_BLEND") != nullptr;
	static const bool experimentalFetchDepth = experimentalFetchBlendAll
		|| std::getenv("REALITYVK_VK_EXPERIMENTAL_FB_FETCH_DEPTH") != nullptr;
	static const bool experimentalFetchColor = experimentalFetchBlendAll
		|| std::getenv("REALITYVK_VK_EXPERIMENTAL_FB_FETCH_COLOR") != nullptr;
	static const bool experimentalDualSource = experimentalFetchBlendAll
		|| std::getenv("REALITYVK_VK_EXPERIMENTAL_DUAL_SOURCE") != nullptr;
	switch (_feature) {
	case graphics::SpecialFeatures::Multisampling:
		return m_maxMsaaLevel > 1;
	case graphics::SpecialFeatures::TextureBarrier:
	case graphics::SpecialFeatures::BlitFramebuffer:
	case graphics::SpecialFeatures::ShaderProgramBinary:
	case graphics::SpecialFeatures::WeakBlitFramebuffer:
	case graphics::SpecialFeatures::ImageTextures:
		return m_coreReady;
	case graphics::SpecialFeatures::N64DepthWithFbFetchDepth:
		return m_coreReady && experimentalFetchDepth;
	case graphics::SpecialFeatures::FramebufferFetchColor:
		return m_coreReady && experimentalFetchColor;
	case graphics::SpecialFeatures::DualSourceBlending:
		// Dual-source factors require backend support and device feature enablement.
		return m_coreReady && experimentalDualSource && m_vk != nullptr && m_vk->dualSrcBlendEnabled;
	case graphics::SpecialFeatures::EglImage:
	case graphics::SpecialFeatures::EglImageFramebuffer:
		return false;
	case graphics::SpecialFeatures::DepthFramebufferTextures:
	case graphics::SpecialFeatures::IntegerTextures:
		return m_coreReady;
	}
	return false;
}

s32 ContextImpl::getMaxMSAALevel()
{
	return m_maxMsaaLevel;
}

bool ContextImpl::isError() const
{
	if (!m_coreReady || !m_vk)
		return true;
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	return m_vk->instance == VK_NULL_HANDLE
		|| m_vk->physicalDevice == VK_NULL_HANDLE
		|| m_vk->device == VK_NULL_HANDLE
		|| m_vk->graphicsQueue == VK_NULL_HANDLE;
#else
	return true;
#endif
}

bool ContextImpl::isFramebufferError() const
{
	if (!m_coreReady || !m_vk)
		return true;
#if REALITYVK_VULKAN_HEADERS_AVAILABLE
	if (m_vk->renderPass == VK_NULL_HANDLE)
		return true;
	const graphics::ObjectHandle drawFramebuffer = m_vk->drawRecorder.drawFramebufferBinding();
	if (drawFramebuffer == graphics::ObjectHandle::defaultFramebuffer) {
		return m_vk->swapchain == VK_NULL_HANDLE
			|| m_vk->swapchainFramebuffers.empty()
			|| m_vk->swapchainExtent.width == 0U
			|| m_vk->swapchainExtent.height == 0U;
	}

	const FramebufferStore::FramebufferResource * framebuffer = m_vk->framebufferStore.getFramebuffer(drawFramebuffer);
	if (framebuffer == nullptr)
		return true;

	const u32 colorAttachmentLimit = m_vk->framebufferStore.getDrawBufferCount(drawFramebuffer);
	const FramebufferStore::FramebufferAttachment * colorAttachment = resolveColorAttachment(
		m_vk->framebufferStore,
		drawFramebuffer,
		graphics::bufferAttachment::COLOR_ATTACHMENT0,
		nullptr,
		colorAttachmentLimit);
	if (colorAttachment != nullptr && colorAttachment->textureHandle.isNotNull()) {
		const TextureStore::TextureResource * colorTexture = m_vk->textureStore.getTexture(colorAttachment->textureHandle);
		if (colorTexture == nullptr || colorTexture->imageView == VK_NULL_HANDLE)
			return true;
		return false;
	}

	const FramebufferStore::FramebufferAttachment * depthAttachment = m_vk->framebufferStore.getAttachment(
		drawFramebuffer,
		graphics::bufferAttachment::DEPTH_ATTACHMENT);
	if (depthAttachment != nullptr && depthAttachment->textureHandle.isNotNull()) {
		const TextureStore::TextureResource * depthTexture = m_vk->textureStore.getTexture(depthAttachment->textureHandle);
		return depthTexture == nullptr || depthTexture->imageView == VK_NULL_HANDLE;
	}
	return true;
#else
	return true;
#endif
}

void ContextImpl::initFramebufferFormats()
{
	m_fbTexFormats.reset(new graphics::FramebufferTextureFormats);
	m_fbTexFormats->colorInternalFormat = graphics::internalcolorFormat::RGBA8;
	m_fbTexFormats->colorFormat = graphics::colorFormat::RGBA;
	m_fbTexFormats->colorType = graphics::datatype::UNSIGNED_BYTE;
	m_fbTexFormats->colorFormatBytes = 4;

	m_fbTexFormats->monochromeInternalFormat = graphics::internalcolorFormat::LUMINANCE;
	m_fbTexFormats->monochromeFormat = graphics::colorFormat::RED;
	m_fbTexFormats->monochromeType = graphics::datatype::UNSIGNED_BYTE;
	m_fbTexFormats->monochromeFormatBytes = 1;

	m_fbTexFormats->depthInternalFormat = graphics::internalcolorFormat::DEPTH;
	m_fbTexFormats->depthFormat = graphics::colorFormat::DEPTH;
	m_fbTexFormats->depthType = graphics::datatype::FLOAT;
	m_fbTexFormats->depthFormatBytes = 4;

	m_fbTexFormats->depthImageInternalFormat = graphics::internalcolorFormat::R16F;
	m_fbTexFormats->depthImageFormat = graphics::colorFormat::RED;
	m_fbTexFormats->depthImageType = graphics::datatype::FLOAT;
	m_fbTexFormats->depthImageFormatBytes = 4;

	m_fbTexFormats->lutInternalFormat = graphics::internalcolorFormat::COLOR_INDEX8;
	m_fbTexFormats->lutFormat = graphics::colorFormat::RED;
	m_fbTexFormats->lutType = graphics::datatype::UNSIGNED_INT;
	m_fbTexFormats->lutFormatBytes = 4;

	m_fbTexFormats->fontInternalFormat = graphics::internalcolorFormat::LUMINANCE;
	m_fbTexFormats->fontFormat = graphics::colorFormat::RED;
	m_fbTexFormats->fontType = graphics::datatype::UNSIGNED_BYTE;
	m_fbTexFormats->fontFormatBytes = 1;
}

graphics::ObjectHandle ContextImpl::_allocateHandle()
{
	return graphics::ObjectHandle(m_nextHandle++);
}

} // namespace vulkan
