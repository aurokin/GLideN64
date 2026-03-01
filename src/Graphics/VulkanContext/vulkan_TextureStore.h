#pragma once

#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <Types.h>
#include <Graphics/Context.h>
#include <Graphics/ObjectHandle.h>
#include <Graphics/Parameters.h>
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

#include "vulkan_TextureUploader.h"

namespace vulkan {

#if REALITYVK_INTERNAL_HAS_VULKAN_HEADERS
class TextureStore
{
public:
	struct TextureResource {
		graphics::ObjectHandle handle = graphics::ObjectHandle::null;
		graphics::TextureTargetParam target = graphics::textureTarget::TEXTURE_2D;
		u32 width = 0;
		u32 height = 0;
		u32 contentWidth = 0;
		u32 contentHeight = 0;
		u32 mipLevels = 1;
		u32 msaaLevel = 0;
		graphics::InternalColorFormatParam internalFormat = graphics::internalcolorFormat::RGBA8;
		graphics::ColorFormatParam format = graphics::colorFormat::RGBA;
		graphics::DatatypeParam type = graphics::datatype::UNSIGNED_BYTE;
		graphics::TextureParam magFilter = graphics::textureParameters::FILTER_LINEAR;
		graphics::TextureParam minFilter = graphics::textureParameters::FILTER_LINEAR;
		graphics::TextureParam wrapS = graphics::textureParameters::WRAP_CLAMP_TO_EDGE;
		graphics::TextureParam wrapT = graphics::textureParameters::WRAP_CLAMP_TO_EDGE;
		graphics::Parameter maxMipmapLevel = graphics::Parameter(0U);
		graphics::Parameter maxAnisotropy = graphics::Parameter(1.0f);
		VkFormat vkFormat = VK_FORMAT_UNDEFINED;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView imageView = VK_NULL_HANDLE;
		VkSampler sampler = VK_NULL_HANDLE;
		VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		bool initialized = false;
		bool hasContent = false;
		bool renderTargetWritten = false;
	};

	bool init(
		VkPhysicalDevice _physicalDevice,
		VkDevice _device,
		VkCommandPool _commandPool,
		VkQueue _graphicsQueue,
		f32 _maxAnisotropy)
	{
		destroy();
		if (_physicalDevice == VK_NULL_HANDLE || _device == VK_NULL_HANDLE)
			return false;
		m_physicalDevice = _physicalDevice;
		m_device = _device;
		m_maxAnisotropy = std::max(1.0f, _maxAnisotropy);
		if (!m_uploader.init(_physicalDevice, _device, _commandPool, _graphicsQueue)) {
			destroy();
			return false;
		}
		return true;
	}

	void destroy()
	{
		if (m_device != VK_NULL_HANDLE) {
			for (auto & item : m_textures)
				_destroyTextureGpu(item.second);
		}
		m_uploader.destroy();
		m_textures.clear();
		m_physicalDevice = VK_NULL_HANDLE;
		m_device = VK_NULL_HANDLE;
		m_maxAnisotropy = 1.0f;
	}

	void createTexture(graphics::ObjectHandle _handle, graphics::Parameter _target)
	{
		if (!_handle.isNotNull())
			return;
		TextureResource & texture = m_textures[static_cast<u32>(_handle)];
		texture.handle = _handle;
		texture.target = graphics::TextureTargetParam(static_cast<u32>(_target));
		texture.initialized = false;
		texture.hasContent = false;
		texture.contentWidth = 0U;
		texture.contentHeight = 0U;
		texture.renderTargetWritten = false;
	}

	void ensureTexture(graphics::ObjectHandle _handle, graphics::TextureTargetParam _target)
	{
		if (!_handle.isNotNull())
			return;
		if (m_textures.find(static_cast<u32>(_handle)) == m_textures.end())
			createTexture(_handle, graphics::Parameter(static_cast<u32>(_target)));
	}

	void deleteTexture(graphics::ObjectHandle _handle)
	{
		if (!_handle.isNotNull())
			return;
		const u32 key = static_cast<u32>(_handle);
		auto it = m_textures.find(key);
		if (it == m_textures.end())
			return;
		_destroyTextureGpu(it->second);
		m_textures.erase(it);
	}

	void initTexture(const graphics::Context::InitTextureParams & _params, s32 _unpackAlignment)
	{
		if (!_params.handle.isNotNull())
			return;
		const u32 key = static_cast<u32>(_params.handle);
		auto it = m_textures.find(key);
		if (it == m_textures.end()) {
			createTexture(_params.handle, _params.target);
			it = m_textures.find(key);
			if (it == m_textures.end())
				return;
		}

		TextureResource & texture = it->second;
		texture.target = _params.target;
		texture.width = _params.width;
		texture.height = _params.height;
		texture.mipLevels = std::max<u32>(1U, _params.mipMapLevels);
		texture.msaaLevel = _params.msaaLevel;
		texture.internalFormat = _params.internalFormat;
		texture.format = _params.format;
		texture.type = _params.dataType;
		texture.hasContent = false;
		texture.contentWidth = 0U;
		texture.contentHeight = 0U;
		texture.renderTargetWritten = false;
		texture.vkFormat = _toVkFormat(_params.internalFormat, _params.format, _params.dataType);
		if (texture.vkFormat == VK_FORMAT_UNDEFINED) {
			LOG(LOG_WARNING, "Unsupported Vulkan texture format combination; texture init skipped.");
			return;
		}
		static const bool debugTextureInit = std::getenv("REALITYVK_VK_DEBUG_TEXTURE_INIT") != nullptr;
		if (debugTextureInit) {
			LOG(
				LOG_WARNING,
				"VK texture init: handle=%u size=%ux%u internal=%u format=%u type=%u mipLevels=%u mipLevel=%u vkFormat=%u hasData=%u",
				static_cast<u32>(_params.handle),
				_params.width,
				_params.height,
				static_cast<u32>(_params.internalFormat),
				static_cast<u32>(_params.format),
				static_cast<u32>(_params.dataType),
				_params.mipMapLevels,
				_params.mipMapLevel,
				static_cast<u32>(texture.vkFormat),
				_params.data != nullptr ? 1U : 0U);
		}
		if (!_recreateTextureStorage(texture))
			return;
		_ensureTextureSampler(texture);

		static const bool debugUploads = std::getenv("REALITYVK_VK_DEBUG_TEXTURE_UPLOAD") != nullptr;
		if (_params.data != nullptr && texture.width > 0 && texture.height > 0) {
			TextureUploader::UploadRequest uploadRequest{};
			uploadRequest.width = _params.width;
			uploadRequest.height = _params.height;
			uploadRequest.mipLevel = _params.mipMapLevel;
			uploadRequest.format = _params.format;
			uploadRequest.type = _params.dataType;
			uploadRequest.data = _params.data;
			uploadRequest.unpackAlignment = _unpackAlignment;
			const bool uploadOk = m_uploader.upload2D(
				texture.image,
				texture.vkFormat,
				texture.mipLevels,
				texture.imageLayout,
				uploadRequest);
			if (debugUploads) {
				static u32 uploadLogCount = 0U;
				if (uploadLogCount < 256U) {
					u32 bytesPerPixel = 4U;
					if (_params.dataType == graphics::datatype::UNSIGNED_SHORT_4_4_4_4
						|| _params.dataType == graphics::datatype::UNSIGNED_SHORT_5_5_5_1
						|| _params.dataType == graphics::datatype::UNSIGNED_SHORT_5_6_5) {
						bytesPerPixel = 2U;
					} else {
						u32 comps = 4U;
						if (_params.format == graphics::colorFormat::RED || _params.format == graphics::colorFormat::LUMINANCE)
							comps = 1U;
						else if (_params.format == graphics::colorFormat::RG)
							comps = 2U;
						else if (_params.format == graphics::colorFormat::RED_GREEN_BLUE)
							comps = 3U;
						u32 bytesPerComp = 1U;
						if (_params.dataType == graphics::datatype::UNSIGNED_SHORT)
							bytesPerComp = 2U;
						else if (_params.dataType == graphics::datatype::UNSIGNED_INT || _params.dataType == graphics::datatype::FLOAT)
							bytesPerComp = 4U;
						bytesPerPixel = comps * bytesPerComp;
					}
					const size_t estimatedBytes = static_cast<size_t>(_params.width) * static_cast<size_t>(_params.height) * bytesPerPixel;
					const size_t sampleBytes = std::min<size_t>(estimatedBytes, 512U);
					const u8 * raw = static_cast<const u8 *>(_params.data);
					u32 nonZeroBytes = 0U;
					for (size_t i = 0; i < sampleBytes; ++i) {
						if (raw[i] != 0U)
							++nonZeroBytes;
					}
					u32 sampleTexels = 0U;
					u32 packedColorNonZero = 0U;
					u32 packedAlphaNonZero = 0U;
					if ((_params.dataType == graphics::datatype::UNSIGNED_SHORT_5_5_5_1
						|| _params.dataType == graphics::datatype::UNSIGNED_SHORT_4_4_4_4)
						&& sampleBytes >= 2U) {
						sampleTexels = static_cast<u32>(sampleBytes / 2U);
						for (u32 i = 0; i < sampleTexels; ++i) {
							const u16 value = static_cast<u16>(raw[i * 2U]) | (static_cast<u16>(raw[i * 2U + 1U]) << 8U);
							if (_params.dataType == graphics::datatype::UNSIGNED_SHORT_5_5_5_1) {
								if ((value >> 1U) != 0U)
									++packedColorNonZero;
								if ((value & 1U) != 0U)
									++packedAlphaNonZero;
							} else {
								if ((value & 0xFFF0U) != 0U)
									++packedColorNonZero;
								if ((value & 0x000FU) != 0U)
									++packedAlphaNonZero;
							}
						}
					}
					LOG(
						LOG_WARNING,
						"VK texture upload(init): handle=%u size=%ux%u internal=%u format=%u type=%u mip=%u ok=%u sampleNonZero=%u/%u packedColorNZ=%u/%u packedAlphaNZ=%u/%u",
						static_cast<u32>(_params.handle),
						_params.width,
						_params.height,
						static_cast<u32>(_params.internalFormat),
						static_cast<u32>(_params.format),
						static_cast<u32>(_params.dataType),
						_params.mipMapLevel,
						uploadOk ? 1U : 0U,
						nonZeroBytes,
						static_cast<u32>(sampleBytes),
						packedColorNonZero,
						sampleTexels,
						packedAlphaNonZero,
						sampleTexels);
					++uploadLogCount;
				}
			}
			if (uploadOk) {
				texture.hasContent = true;
				texture.renderTargetWritten = false;
				if (_params.mipMapLevel == 0U) {
					texture.contentWidth = std::min(texture.width, _params.width);
					texture.contentHeight = std::min(texture.height, _params.height);
				}
			}
		} else {
			m_uploader.transitionToShaderRead(
				texture.image,
				texture.vkFormat,
				texture.mipLevels,
				texture.imageLayout);
			texture.hasContent = false;
			texture.contentWidth = 0U;
			texture.contentHeight = 0U;
			texture.renderTargetWritten = false;
		}

		texture.initialized = true;
	}

	void updateTexture(const graphics::Context::UpdateTextureDataParams & _params, s32 _unpackAlignment)
	{
		if (!_params.handle.isNotNull() || _params.data == nullptr || _params.width == 0 || _params.height == 0)
			return;
		auto it = m_textures.find(static_cast<u32>(_params.handle));
		if (it == m_textures.end())
			return;
		TextureResource & texture = it->second;
		if (texture.image == VK_NULL_HANDLE)
			return;
		static const bool debugTextureInit = std::getenv("REALITYVK_VK_DEBUG_TEXTURE_INIT") != nullptr;
		if (debugTextureInit) {
			LOG(
				LOG_WARNING,
				"VK texture update: handle=%u dstVkFormat=%u dstInternal=%u dstFormat=%u dstType=%u uploadFormat=%u uploadType=%u offset=%u,%u size=%ux%u mip=%u",
				static_cast<u32>(_params.handle),
				static_cast<u32>(texture.vkFormat),
				static_cast<u32>(texture.internalFormat),
				static_cast<u32>(texture.format),
				static_cast<u32>(texture.type),
				static_cast<u32>(_params.format),
				static_cast<u32>(_params.dataType),
				_params.x,
				_params.y,
				_params.width,
				_params.height,
				_params.mipMapLevel);
		}

		TextureUploader::UploadRequest uploadRequest{};
		uploadRequest.x = _params.x;
		uploadRequest.y = _params.y;
		uploadRequest.width = _params.width;
		uploadRequest.height = _params.height;
		uploadRequest.mipLevel = _params.mipMapLevel;
		uploadRequest.format = _params.format;
		uploadRequest.type = _params.dataType;
		uploadRequest.data = _params.data;
		uploadRequest.unpackAlignment = _unpackAlignment;
		const bool uploadOk = m_uploader.upload2D(
			texture.image,
			texture.vkFormat,
			texture.mipLevels,
			texture.imageLayout,
			uploadRequest);
		if (uploadOk) {
			texture.hasContent = true;
			texture.renderTargetWritten = false;
			if (_params.mipMapLevel == 0U) {
				const u64 right = static_cast<u64>(_params.x) + static_cast<u64>(_params.width);
				const u64 bottom = static_cast<u64>(_params.y) + static_cast<u64>(_params.height);
				texture.contentWidth = std::max(texture.contentWidth, static_cast<u32>(std::min<u64>(right, texture.width)));
				texture.contentHeight = std::max(texture.contentHeight, static_cast<u32>(std::min<u64>(bottom, texture.height)));
			}
		}
		static const bool debugUploads = std::getenv("REALITYVK_VK_DEBUG_TEXTURE_UPLOAD") != nullptr;
		if (debugUploads) {
			static u32 updateLogCount = 0U;
			if (updateLogCount < 256U) {
				u32 grayscalePixels = 0U;
				u32 coloredPixels = 0U;
				u32 alphaNonOpaquePixels = 0U;
				u32 sampledPixels = 0U;
				if (_params.format == graphics::colorFormat::RGBA
					&& _params.dataType == graphics::datatype::UNSIGNED_BYTE
					&& _params.data != nullptr) {
					const u8 * rgba = static_cast<const u8 *>(_params.data);
					sampledPixels = static_cast<u32>(std::min<u64>(
						static_cast<u64>(_params.width) * static_cast<u64>(_params.height),
						256U));
					for (u32 i = 0; i < sampledPixels; ++i) {
						const u8 r = rgba[i * 4U + 0U];
						const u8 g = rgba[i * 4U + 1U];
						const u8 b = rgba[i * 4U + 2U];
						const u8 a = rgba[i * 4U + 3U];
						if (r == g && g == b)
							++grayscalePixels;
						else
							++coloredPixels;
						if (a != 255U)
							++alphaNonOpaquePixels;
					}
				}
				LOG(
					LOG_WARNING,
					"VK texture upload(update): handle=%u offset=%u,%u size=%ux%u format=%u type=%u mip=%u ok=%u samplePixels=%u grayscale=%u colored=%u alphaNonOpaque=%u",
					static_cast<u32>(_params.handle),
					_params.x,
					_params.y,
					_params.width,
					_params.height,
					static_cast<u32>(_params.format),
					static_cast<u32>(_params.dataType),
					_params.mipMapLevel,
					uploadOk ? 1U : 0U,
					sampledPixels,
					grayscalePixels,
					coloredPixels,
					alphaNonOpaquePixels);
				++updateLogCount;
			}
		}
	}

	void setTextureParameters(const graphics::Context::TexParameters & _parameters)
	{
		if (!_parameters.handle.isNotNull())
			return;
		auto it = m_textures.find(static_cast<u32>(_parameters.handle));
		if (it == m_textures.end())
			return;
		TextureResource & texture = it->second;
		const bool unchanged = texture.magFilter == _parameters.magFilter
			&& texture.minFilter == _parameters.minFilter
			&& texture.wrapS == _parameters.wrapS
			&& texture.wrapT == _parameters.wrapT
			&& texture.maxMipmapLevel == _parameters.maxMipmapLevel
			&& texture.maxAnisotropy == _parameters.maxAnisotropy;
		if (unchanged)
			return;
		texture.magFilter = _parameters.magFilter;
		texture.minFilter = _parameters.minFilter;
		texture.wrapS = _parameters.wrapS;
		texture.wrapT = _parameters.wrapT;
		texture.maxMipmapLevel = _parameters.maxMipmapLevel;
		texture.maxAnisotropy = _parameters.maxAnisotropy;
		_ensureTextureSampler(texture);
	}

	const TextureResource * getTexture(graphics::ObjectHandle _handle) const
	{
		auto it = m_textures.find(static_cast<u32>(_handle));
		if (it == m_textures.end())
			return nullptr;
		return &it->second;
	}

	TextureResource * getTextureMutable(graphics::ObjectHandle _handle)
	{
		auto it = m_textures.find(static_cast<u32>(_handle));
		if (it == m_textures.end())
			return nullptr;
		return &it->second;
	}

	bool clearTextureColor(graphics::ObjectHandle _handle, f32 _r, f32 _g, f32 _b, f32 _a)
	{
		auto it = m_textures.find(static_cast<u32>(_handle));
		if (it == m_textures.end())
			return false;
		TextureResource & texture = it->second;
		if (texture.image == VK_NULL_HANDLE)
			return false;
		const bool cleared = m_uploader.clearColor(
			texture.image,
			texture.vkFormat,
			texture.mipLevels,
			texture.imageLayout,
			_r,
			_g,
			_b,
			_a);
		if (cleared)
			texture.hasContent = true;
		if (cleared) {
			texture.contentWidth = texture.width;
			texture.contentHeight = texture.height;
			texture.renderTargetWritten = true;
		}
		return cleared;
	}

	bool clearTextureDepth(graphics::ObjectHandle _handle, f32 _depth)
	{
		auto it = m_textures.find(static_cast<u32>(_handle));
		if (it == m_textures.end())
			return false;
		TextureResource & texture = it->second;
		if (texture.image == VK_NULL_HANDLE)
			return false;
		const bool cleared = m_uploader.clearDepth(
			texture.image,
			texture.vkFormat,
			texture.mipLevels,
			texture.imageLayout,
			_depth);
		if (cleared)
			texture.hasContent = true;
		if (cleared) {
			texture.contentWidth = texture.width;
			texture.contentHeight = texture.height;
			texture.renderTargetWritten = true;
		}
		return cleared;
	}

	bool blitTexture(
		graphics::ObjectHandle _srcHandle,
		graphics::ObjectHandle _dstHandle,
		s32 _srcX0,
		s32 _srcY0,
		s32 _srcX1,
		s32 _srcY1,
		s32 _dstX0,
		s32 _dstY0,
		s32 _dstX1,
		s32 _dstY1,
		graphics::TextureParam _filter,
		bool _depthCopy)
	{
		auto srcIt = m_textures.find(static_cast<u32>(_srcHandle));
		auto dstIt = m_textures.find(static_cast<u32>(_dstHandle));
		if (srcIt == m_textures.end() || dstIt == m_textures.end())
			return false;
		TextureResource & src = srcIt->second;
		TextureResource & dst = dstIt->second;
		if (src.image == VK_NULL_HANDLE || dst.image == VK_NULL_HANDLE)
			return false;
		if (!_depthCopy && src.vkFormat != dst.vkFormat)
			return false;

		auto clampToDimension = [](s32 _coord, u32 _dimension) -> s32 {
			const s32 maxCoord = static_cast<s32>(_dimension);
			if (_coord < 0)
				return 0;
			if (_coord > maxCoord)
				return maxCoord;
			return _coord;
		};
		_srcX0 = clampToDimension(_srcX0, src.width);
		_srcY0 = clampToDimension(_srcY0, src.height);
		_srcX1 = clampToDimension(_srcX1, src.width);
		_srcY1 = clampToDimension(_srcY1, src.height);
		_dstX0 = clampToDimension(_dstX0, dst.width);
		_dstY0 = clampToDimension(_dstY0, dst.height);
		_dstX1 = clampToDimension(_dstX1, dst.width);
		_dstY1 = clampToDimension(_dstY1, dst.height);

		TextureUploader::BlitRequest blitRequest{};
		blitRequest.srcX0 = _srcX0;
		blitRequest.srcY0 = _srcY0;
		blitRequest.srcX1 = _srcX1;
		blitRequest.srcY1 = _srcY1;
		blitRequest.dstX0 = _dstX0;
		blitRequest.dstY0 = _dstY0;
		blitRequest.dstX1 = _dstX1;
		blitRequest.dstY1 = _dstY1;
		blitRequest.linearFilter = _filter == graphics::textureParameters::FILTER_LINEAR;
		blitRequest.depthCopy = _depthCopy;

		const bool blitOk = m_uploader.blit2D(
			src.image,
			src.vkFormat,
			src.mipLevels,
			src.imageLayout,
			dst.image,
			dst.vkFormat,
			dst.mipLevels,
			dst.imageLayout,
			blitRequest);
		if (blitOk) {
			dst.hasContent = true;
			dst.renderTargetWritten = true;
			const s32 maxDstX = std::max(_dstX0, _dstX1);
			const s32 maxDstY = std::max(_dstY0, _dstY1);
			if (maxDstX > 0)
				dst.contentWidth = std::max(dst.contentWidth, static_cast<u32>(maxDstX));
			if (maxDstY > 0)
				dst.contentHeight = std::max(dst.contentHeight, static_cast<u32>(maxDstY));
		}
		return blitOk;
	}

	bool readTexture(
		graphics::ObjectHandle _handle,
		u32 _x,
		u32 _y,
		u32 _width,
		u32 _height,
		std::vector<u8> & _outBytes,
		u32 & _outRowBytes,
		u32 & _outBytesPerPixel)
	{
		_outBytes.clear();
		_outRowBytes = 0U;
		_outBytesPerPixel = 0U;

		auto it = m_textures.find(static_cast<u32>(_handle));
		if (it == m_textures.end())
			return false;
		TextureResource & texture = it->second;
		if (texture.image == VK_NULL_HANDLE
			|| texture.vkFormat == VK_FORMAT_UNDEFINED
			|| texture.width == 0U
			|| texture.height == 0U) {
			return false;
		}
		if (_x >= texture.width || _y >= texture.height)
			return false;

		const u32 readWidth = std::min(_width, texture.width - _x);
		const u32 readHeight = std::min(_height, texture.height - _y);
		if (readWidth == 0U || readHeight == 0U)
			return false;

		TextureUploader::ReadbackRequest readbackRequest{};
		readbackRequest.x = _x;
		readbackRequest.y = _y;
		readbackRequest.width = readWidth;
		readbackRequest.height = readHeight;
		readbackRequest.mipLevel = 0U;
		return m_uploader.readback2D(
			texture.image,
			texture.vkFormat,
			texture.mipLevels,
			texture.imageLayout,
			readbackRequest,
			_outBytes,
			_outRowBytes,
			_outBytesPerPixel);
	}

private:
	static VkFilter _toVkFilter(graphics::TextureParam _param)
	{
		if (_param == graphics::textureParameters::FILTER_NEAREST
			|| _param == graphics::textureParameters::FILTER_NEAREST_MIPMAP_NEAREST) {
			return VK_FILTER_NEAREST;
		}
		return VK_FILTER_LINEAR;
	}

	static VkSamplerMipmapMode _toVkMipmapMode(graphics::TextureParam _param)
	{
		if (_param == graphics::textureParameters::FILTER_NEAREST_MIPMAP_NEAREST
			|| _param == graphics::textureParameters::FILTER_LINEAR_MIPMAP_NEAREST) {
			return VK_SAMPLER_MIPMAP_MODE_NEAREST;
		}
		return VK_SAMPLER_MIPMAP_MODE_LINEAR;
	}

	static VkSamplerAddressMode _toVkAddressMode(graphics::TextureParam _param)
	{
		if (_param == graphics::textureParameters::WRAP_REPEAT)
			return VK_SAMPLER_ADDRESS_MODE_REPEAT;
		if (_param == graphics::textureParameters::WRAP_MIRRORED_REPEAT)
			return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	}

	static VkFormat _toVkFormat(
		graphics::InternalColorFormatParam _internalFormat,
		graphics::ColorFormatParam _format,
		graphics::DatatypeParam _type)
	{
		if (_internalFormat == graphics::internalcolorFormat::RGBA8
			|| _internalFormat == graphics::internalcolorFormat::RGBA4
			|| _internalFormat == graphics::internalcolorFormat::RGB5_A1) {
			return VK_FORMAT_R8G8B8A8_UNORM;
		}
		if (_internalFormat == graphics::internalcolorFormat::RGB8) {
			return VK_FORMAT_R8G8B8A8_UNORM;
		}
		if (_internalFormat == graphics::internalcolorFormat::RG)
			return VK_FORMAT_R8G8_UNORM;
		if (_internalFormat == graphics::internalcolorFormat::R16F)
			return VK_FORMAT_R16_SFLOAT;
		if (_internalFormat == graphics::internalcolorFormat::RG32F)
			return VK_FORMAT_R32G32_SFLOAT;
		if (_internalFormat == graphics::internalcolorFormat::LUMINANCE)
			return VK_FORMAT_R8_UNORM;
		if (_internalFormat == graphics::internalcolorFormat::COLOR_INDEX8) {
			if (_type == graphics::datatype::UNSIGNED_INT)
				return VK_FORMAT_R32_UINT;
			if (_type == graphics::datatype::UNSIGNED_BYTE)
				return VK_FORMAT_R8_UINT;
			return VK_FORMAT_UNDEFINED;
		}
		if (_internalFormat == graphics::internalcolorFormat::DEPTH)
			return VK_FORMAT_D32_SFLOAT;

		if (_format == graphics::colorFormat::RGBA && _type == graphics::datatype::UNSIGNED_BYTE)
			return VK_FORMAT_R8G8B8A8_UNORM;
		if (_format == graphics::colorFormat::RED && _type == graphics::datatype::UNSIGNED_INT)
			return VK_FORMAT_R32_UINT;
		if (_format == graphics::colorFormat::RG && _type == graphics::datatype::UNSIGNED_BYTE)
			return VK_FORMAT_R8G8_UNORM;
		if (_format == graphics::colorFormat::RED && _type == graphics::datatype::UNSIGNED_BYTE)
			return VK_FORMAT_R8_UNORM;
		return VK_FORMAT_UNDEFINED;
	}

	u32 _findMemoryType(u32 _typeFilter, VkMemoryPropertyFlags _requiredProperties) const
	{
		if (m_physicalDevice == VK_NULL_HANDLE)
			return UINT32_MAX;

		VkPhysicalDeviceMemoryProperties memoryProperties{};
		vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memoryProperties);
		for (u32 i = 0; i < memoryProperties.memoryTypeCount; ++i) {
			if ((_typeFilter & (1U << i)) == 0U)
				continue;
			if ((memoryProperties.memoryTypes[i].propertyFlags & _requiredProperties) == _requiredProperties)
				return i;
		}
		return UINT32_MAX;
	}

	void _destroyTextureGpu(TextureResource & _texture)
	{
		if (m_device == VK_NULL_HANDLE)
			return;
		if (_texture.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(m_device, _texture.sampler, nullptr);
			_texture.sampler = VK_NULL_HANDLE;
		}
		if (_texture.imageView != VK_NULL_HANDLE) {
			vkDestroyImageView(m_device, _texture.imageView, nullptr);
			_texture.imageView = VK_NULL_HANDLE;
		}
		if (_texture.image != VK_NULL_HANDLE) {
			vkDestroyImage(m_device, _texture.image, nullptr);
			_texture.image = VK_NULL_HANDLE;
		}
		if (_texture.memory != VK_NULL_HANDLE) {
			vkFreeMemory(m_device, _texture.memory, nullptr);
			_texture.memory = VK_NULL_HANDLE;
		}
		_texture.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		_texture.initialized = false;
		_texture.hasContent = false;
		_texture.contentWidth = 0U;
		_texture.contentHeight = 0U;
		_texture.renderTargetWritten = false;
	}

	bool _recreateTextureStorage(TextureResource & _texture)
	{
		if (m_device == VK_NULL_HANDLE || m_physicalDevice == VK_NULL_HANDLE)
			return false;

		_destroyTextureGpu(_texture);
		if (_texture.width == 0 || _texture.height == 0 || _texture.vkFormat == VK_FORMAT_UNDEFINED)
			return false;

		VkImageCreateInfo imageCreateInfo{};
		imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
		imageCreateInfo.extent.width = _texture.width;
		imageCreateInfo.extent.height = _texture.height;
		imageCreateInfo.extent.depth = 1;
		imageCreateInfo.mipLevels = std::max<u32>(1U, _texture.mipLevels);
		imageCreateInfo.arrayLayers = 1;
		imageCreateInfo.format = _texture.vkFormat;
		imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		imageCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		if (_texture.vkFormat == VK_FORMAT_D32_SFLOAT || _texture.vkFormat == VK_FORMAT_D16_UNORM)
			imageCreateInfo.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		if (vkCreateImage(m_device, &imageCreateInfo, nullptr, &_texture.image) != VK_SUCCESS)
			return false;

		VkMemoryRequirements memoryRequirements{};
		vkGetImageMemoryRequirements(m_device, _texture.image, &memoryRequirements);
		const u32 memoryTypeIndex = _findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (memoryTypeIndex == UINT32_MAX) {
			_destroyTextureGpu(_texture);
			return false;
		}

		VkMemoryAllocateInfo allocateInfo{};
		allocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocateInfo.allocationSize = memoryRequirements.size;
		allocateInfo.memoryTypeIndex = memoryTypeIndex;
		if (vkAllocateMemory(m_device, &allocateInfo, nullptr, &_texture.memory) != VK_SUCCESS) {
			_destroyTextureGpu(_texture);
			return false;
		}

		if (vkBindImageMemory(m_device, _texture.image, _texture.memory, 0) != VK_SUCCESS) {
			_destroyTextureGpu(_texture);
			return false;
		}

		VkImageViewCreateInfo imageViewCreateInfo{};
		imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		imageViewCreateInfo.image = _texture.image;
		imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageViewCreateInfo.format = _texture.vkFormat;
		imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
		imageViewCreateInfo.subresourceRange.levelCount = std::max<u32>(1U, _texture.mipLevels);
		imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
		imageViewCreateInfo.subresourceRange.layerCount = 1;
		imageViewCreateInfo.subresourceRange.aspectMask = (_texture.vkFormat == VK_FORMAT_D32_SFLOAT || _texture.vkFormat == VK_FORMAT_D16_UNORM)
			? VK_IMAGE_ASPECT_DEPTH_BIT
			: VK_IMAGE_ASPECT_COLOR_BIT;
		if (vkCreateImageView(m_device, &imageViewCreateInfo, nullptr, &_texture.imageView) != VK_SUCCESS) {
			_destroyTextureGpu(_texture);
			return false;
		}

		_texture.imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		return true;
	}

	bool _ensureTextureSampler(TextureResource & _texture)
	{
		if (m_device == VK_NULL_HANDLE)
			return false;
		if (_texture.sampler != VK_NULL_HANDLE) {
			vkDestroySampler(m_device, _texture.sampler, nullptr);
			_texture.sampler = VK_NULL_HANDLE;
		}

		VkSamplerCreateInfo samplerCreateInfo{};
		samplerCreateInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerCreateInfo.magFilter = _toVkFilter(_texture.magFilter);
		samplerCreateInfo.minFilter = _toVkFilter(_texture.minFilter);
		samplerCreateInfo.mipmapMode = _toVkMipmapMode(_texture.minFilter);
		samplerCreateInfo.addressModeU = _toVkAddressMode(_texture.wrapS);
		samplerCreateInfo.addressModeV = _toVkAddressMode(_texture.wrapT);
		samplerCreateInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerCreateInfo.mipLodBias = 0.0f;
		const bool anisotropyRequested = _texture.maxAnisotropy.isValid() && static_cast<f32>(_texture.maxAnisotropy) > 1.0f;
		samplerCreateInfo.anisotropyEnable = (anisotropyRequested && m_maxAnisotropy > 1.0f) ? VK_TRUE : VK_FALSE;
		samplerCreateInfo.maxAnisotropy = samplerCreateInfo.anisotropyEnable == VK_TRUE
			? std::min(m_maxAnisotropy, std::max(1.0f, static_cast<f32>(_texture.maxAnisotropy)))
			: 1.0f;
		samplerCreateInfo.compareEnable = VK_FALSE;
		samplerCreateInfo.compareOp = VK_COMPARE_OP_ALWAYS;
		samplerCreateInfo.minLod = 0.0f;
		if (_texture.maxMipmapLevel.isValid())
			samplerCreateInfo.maxLod = std::max(0.0f, static_cast<f32>(static_cast<u32>(_texture.maxMipmapLevel)));
		else
			samplerCreateInfo.maxLod = static_cast<f32>(std::max<u32>(1U, _texture.mipLevels) - 1U);
		samplerCreateInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
		samplerCreateInfo.unnormalizedCoordinates = VK_FALSE;

		if (vkCreateSampler(m_device, &samplerCreateInfo, nullptr, &_texture.sampler) != VK_SUCCESS)
			return false;
		return true;
	}

	VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
	VkDevice m_device = VK_NULL_HANDLE;
	f32 m_maxAnisotropy = 1.0f;
	TextureUploader m_uploader;
	std::unordered_map<u32, TextureResource> m_textures;
};
#else
class TextureStore
{
};
#endif

} // namespace vulkan
